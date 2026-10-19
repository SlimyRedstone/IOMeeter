/*
 * VBAN, the VB-Audio network protocol, as much of it as Voicemeeter needs to
 * be told where to put a fader.
 *
 * VBAN carries four sub-protocols over one UDP port. Three of them are
 * streams -- audio, serial, service -- and the fourth, TEXT, is a remote
 * control channel: the payload is a line of Voicemeeter's own script, the same
 * language its macro buttons are written in, and "Strip[0].Gain=-6.0;" moves
 * the first strip's fader to six decibels down. That is the whole of what is
 * implemented here. Nothing is received, nothing is decoded, and no audio
 * crosses this file.
 *
 * Voicemeeter has to be listening: its VBAN dialog must be on, with an
 * incoming TEXT stream whose name matches the one configured here. The name is
 * the match, not the port -- several streams share the one port and the
 * sixteen bytes of name in the header are what tell them apart.
 *
 * One socket, created at startup and kept. The packets are small, they go out
 * unacknowledged, and a fader moved with nothing listening costs one datagram
 * dropped by the loopback device.
 */

#ifndef VBAN_H
#define VBAN_H

#include <stdbool.h>

/* The protocol's own numbers. The header is fixed at 28 bytes and the payload
   may not take the datagram past 1464. */
#define VBAN_HEADER_SIZE  28
#define VBAN_PAYLOAD_MAX  1436

/* Sixteen bytes in the header, so sixteen characters and a terminator. */
#define VBAN_NAME_MAX     17
#define VBAN_IP_MAX       64

/*
 * The registered VBAN port. VB-Audio's own tools default to it and so does
 * Voicemeeter, so a stock installation needs nothing typed in.
 */
#define VBAN_PORT_DEFAULT 6980
#define VBAN_PORT_MIN     1
#define VBAN_PORT_MAX     65535

/* Voicemeeter's first incoming TEXT stream is called this out of the box. */
#define VBAN_NAME_DEFAULT "Command1"

/* The same machine, which is where Voicemeeter is in every case but a studio
   with a second PC taking the audio. */
#define VBAN_IP_DEFAULT   "127.0.0.1"

/*
 * The whole travel of a Voicemeeter fader, end to end.
 *
 * The hardware fader is the strip's fader rather than a volume control over
 * it: the bottom of its travel is the -60 Voicemeeter treats as off and the
 * top is the +12 its own knob reaches, so the two read the same number and a
 * position on one is the same position on the other. Unity is not at the top
 * but five sixths of the way up it, which is where Voicemeeter's 0 mark is.
 */
#define VBAN_DB_MIN (-60.0f)
#define VBAN_DB_MAX (12.0f)

/*
 * How many strips there are to choose between.
 *
 * Potato's count: five hardware inputs and three virtual ones. Banana has
 * five in all and the standard edition three, and a strip chosen past the end
 * of a smaller installation is a command it quietly ignores.
 */
#define VBAN_STRIP_COUNT 8

/**
 * Open the socket.
 *
 * @return false when there is no socket to be had, after which every send is a
 *         no-op and vban_last_error() says why.
 */
bool vban_start(void);

void vban_stop(void);

/** Whether there is a socket and the passthrough is switched on. */
bool vban_available(void);

/** Why nothing is being sent, for logging. Never NULL. */
const char *vban_last_error(void);

/**
 * The settings the interface owns, pushed down rather than read from here.
 *
 * Cheap to call every frame, which is what the main loop does: the address is
 * only re-resolved when the text of it actually changes.
 *
 * @param enabled Whether the passthrough is on at all.
 * @param ip      Dotted quad. Names are not resolved: a lookup would block the
 *                frame, and the far end is the same machine in every ordinary
 *                case.
 * @param port    1..65535; anything else is taken as the default.
 * @param name    Stream name, as Voicemeeter's VBAN dialog spells it.
 */
void vban_configure(bool enabled, const char *ip, int port, const char *name);

/**
 * Send one line of Voicemeeter script.
 *
 * @param command For instance "Strip[0].Gain=-6.0;". Sent as it stands.
 * @return false if it could not be sent.
 */
bool vban_send_text(const char *command);

/**
 * Put strip @p strip at @p db.
 *
 * @param strip Index into Voicemeeter's strips, counted as its own script
 *              counts them: the hardware inputs first, then the virtual ones.
 *              0..VBAN_STRIP_COUNT-1.
 * @param db    Clamped to VBAN_DB_MIN..VBAN_DB_MAX.
 */
bool vban_set_strip_gain(int strip, float db);

/**
 * A fader's 0..@p max position as the same position on Voicemeeter's fader.
 *
 * Absolute, not relative: the bottom of the travel is VBAN_DB_MIN and the top
 * is VBAN_DB_MAX, with everything between them in proportion.
 */
float vban_fader_to_db(int value, int max);

#endif /* VBAN_H */
