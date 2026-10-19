#include "vban.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
/* inet_pton arrived with Vista, and MinGW hides it behind the version macro
   the same way it hides everything else. foreground.c asks for the same
   floor. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>        /* SIO_UDP_CONNRESET */

typedef SOCKET vban_sock_t;
#define VBAN_INVALID  INVALID_SOCKET
#define vban_close    closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

typedef int vban_sock_t;
#define VBAN_INVALID  (-1)
#define vban_close    close
#endif

/* ------------------------------------------------------------ header ---- */

/*
 * Byte 4 carries the sub-protocol in its top three bits and a rate index in
 * the low five. TEXT uses the rate field for a baud rate it does not need, so
 * it is left at zero.
 */
#define VBAN_PROTOCOL_TXT 0x40

/* Byte 7, for TEXT, is the character encoding. ASCII is zero, which is what
   Voicemeeter's script is written in. */
#define VBAN_CODEC_ASCII  0x00

/* ------------------------------------------------------------- state ---- */

static char s_error[200] = "not started";

static vban_sock_t s_socket = VBAN_INVALID;
static bool        s_enabled;

/* The address as it was last resolved, and the text it was resolved from, so
   the work is skipped on the frames where nothing was typed. */
static struct sockaddr_in s_dest;
static bool s_dest_valid;
static char s_ip[VBAN_IP_MAX] = VBAN_IP_DEFAULT;
static int  s_port = VBAN_PORT_DEFAULT;
static char s_name[VBAN_NAME_MAX] = VBAN_NAME_DEFAULT;

/*
 * The frame counter, which VBAN increments per packet per stream. A receiver
 * uses it to notice a gap; Voicemeeter does nothing with it on a TEXT stream
 * but the field is part of the header and a constant there would be wrong.
 */
static unsigned long s_frame;

const char *vban_last_error(void)
{
    return s_error;
}

bool vban_available(void)
{
    return s_socket != VBAN_INVALID && s_enabled && s_dest_valid;
}

/* strnlen is not C99, and the name is bounded by the header's sixteen bytes
   whatever the caller passed. */
static size_t vban_name_len(const char *name)
{
    size_t n = 0;

    while (n < 16 && name[n] != 0) {
        n++;
    }
    return n;
}

/*
 * Lay out one TEXT packet.
 *
 * Written a byte at a time rather than through a packed struct: the layout is
 * the protocol's, not the compiler's, and a struct would need a pragma on
 * every toolchain that builds this.
 *
 * @return the length of the datagram, or -1 if it would not fit.
 */
static int vban_build(unsigned char *out, size_t cap, const char *text)
{
    size_t len = strlen(text);

    if (len > VBAN_PAYLOAD_MAX) {
        len = VBAN_PAYLOAD_MAX;
    }
    if (cap < VBAN_HEADER_SIZE + len) {
        return -1;
    }

    memcpy(out, "VBAN", 4);

    out[4] = VBAN_PROTOCOL_TXT;     /* sub-protocol, no rate */
    out[5] = 0;                     /* samples per frame: unused by TEXT */
    out[6] = 0;                     /* channels: unused by TEXT */
    out[7] = VBAN_CODEC_ASCII;

    /* Sixteen bytes, null padded. Voicemeeter matches the whole field, so the
       padding has to be zero rather than whatever was here before. */
    memset(out + 8, 0, 16);
    memcpy(out + 8, s_name, vban_name_len(s_name));

    /* Little endian on the wire, spelled out so it does not depend on the
       byte order of the machine building it. */
    out[24] = (unsigned char)(s_frame & 0xffu);
    out[25] = (unsigned char)((s_frame >> 8) & 0xffu);
    out[26] = (unsigned char)((s_frame >> 16) & 0xffu);
    out[27] = (unsigned char)((s_frame >> 24) & 0xffu);

    memcpy(out + VBAN_HEADER_SIZE, text, len);

    return (int)(VBAN_HEADER_SIZE + len);
}

/* ------------------------------------------------------------ socket ---- */

bool vban_start(void)
{
#ifdef _WIN32
    /*
     * Nothing else in the program opens a socket, so this is the one place
     * Winsock is brought up. Reference counted either way, which is why the
     * matching WSACleanup() in vban_stop() is safe whatever is added later.
     */
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(s_error, sizeof(s_error), "winsock could not be started");
        return false;
    }
#endif

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket == VBAN_INVALID) {
        snprintf(s_error, sizeof(s_error), "no UDP socket for VBAN");
        return false;
    }

#ifdef _WIN32
    /*
     * With nothing listening, the loopback device answers each datagram with
     * an ICMP port unreachable, and Windows turns that into a WSAECONNRESET on
     * the *next* call against this socket. Every other platform ignores the
     * ICMP on an unconnected socket. Turning the behaviour off is what keeps
     * "Voicemeeter is not running" from reading as "the socket broke".
     */
#ifdef SIO_UDP_CONNRESET
    {
        DWORD off = 0;
        DWORD returned = 0;

        WSAIoctl(s_socket, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0,
                 &returned, NULL, NULL);
    }
#endif
#endif

    s_error[0] = 0;
    return true;
}

void vban_stop(void)
{
    if (s_socket != VBAN_INVALID) {
        vban_close(s_socket);
        s_socket = VBAN_INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    s_dest_valid = false;
    s_enabled = false;
}

void vban_configure(bool enabled, const char *ip, int port, const char *name)
{
    s_enabled = enabled;

    if (name != NULL && strcmp(name, s_name) != 0) {
        snprintf(s_name, sizeof(s_name), "%s", name);
    }

    if (port < VBAN_PORT_MIN || port > VBAN_PORT_MAX) {
        port = VBAN_PORT_DEFAULT;
    }

    bool moved = (port != s_port);

    if (ip != NULL && strcmp(ip, s_ip) != 0) {
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
        moved = true;
    }
    s_port = port;

    if (!moved && s_dest_valid) {
        return;         /* nothing typed since the last pass */
    }

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons((unsigned short)s_port);

    /* Numeric only. A name would mean a lookup, and a lookup on the frame
       somebody is still typing the address into blocks the interface. */
    if (inet_pton(AF_INET, s_ip, &s_dest.sin_addr) != 1) {
        s_dest_valid = false;
        snprintf(s_error, sizeof(s_error),
                 "\"%s\" is not an address; VBAN wants a numeric one such as "
                 VBAN_IP_DEFAULT, s_ip);
        return;
    }

    s_dest_valid = true;
    s_error[0] = 0;
}

/* ------------------------------------------------------------ sending --- */

bool vban_send_text(const char *command)
{
    unsigned char packet[VBAN_HEADER_SIZE + VBAN_PAYLOAD_MAX];

    if (!vban_available() || command == NULL || command[0] == 0) {
        return false;
    }

    int len = vban_build(packet, sizeof(packet), command);

    if (len < 0) {
        return false;
    }

    /* The length goes across unadorned: Windows types this parameter int and
       everything else types it size_t, and a cast would be wrong on one of
       them whichever one it was written for. */
    int sent = (int)sendto(s_socket, (const char *)packet, len, 0,
                           (const struct sockaddr *)&s_dest, sizeof(s_dest));

    if (sent != len) {
        snprintf(s_error, sizeof(s_error), "VBAN packet could not be sent");
        return false;
    }

    s_frame++;
    return true;
}

float vban_fader_to_db(int value, int max)
{
    if (max <= 0 || value <= 0) {
        return VBAN_DB_MIN;
    }
    if (value >= max) {
        return VBAN_DB_MAX;
    }

    /*
     * Straight along the decibel scale rather than along the amplitude, which
     * is what a fader is: Voicemeeter's own strip is marked in decibels and
     * the knob should land where the marks say it will.
     */
    float t = (float)value / (float)max;

    return VBAN_DB_MIN + t * (VBAN_DB_MAX - VBAN_DB_MIN);
}

bool vban_set_strip_gain(int strip, float db)
{
    char command[64];

    if (strip < 0) {
        return false;
    }
    if (db < VBAN_DB_MIN) {
        db = VBAN_DB_MIN;
    }
    if (db > VBAN_DB_MAX) {
        db = VBAN_DB_MAX;
    }

    /* One decimal: Voicemeeter shows the strip to a tenth and the rest would
       be lost on the way in. */
    snprintf(command, sizeof(command), "Strip[%d].Gain=%.1f;", strip,
             (double)db);

    return vban_send_text(command);
}
