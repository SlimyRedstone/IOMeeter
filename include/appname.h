/*
 * Naming an application, the same way everywhere.
 *
 * Four parts of the program identify one: a fader mixes it, a profile is
 * selected by it being in front, a key toggles its playback and a macro is
 * aimed at its window. All four compare what somebody typed against what is
 * running, and they have to agree -- a name that finds a window but not an
 * audio session, or the other way round, is a bug nobody can explain from the
 * outside.
 *
 * So the rule lives here once. Two names mean the same program when they
 * match after the path in front is dropped, ignoring case, and ignoring a
 * suffix that means "this is a program":
 *
 *   chrome  Chrome.exe  C:/Program Files/.../chrome.exe      one program
 *
 * Only those suffixes are dropped, never simply whatever follows the last dot,
 * so a version in the name survives and stays part of it:
 *
 *   python3.11  and  python3.9                               two programs
 */

#ifndef APPNAME_H
#define APPNAME_H

#include <stdbool.h>
#include <stddef.h>

/** The file name at the end of @p path, which is all a program is known by. */
const char *appname_base(const char *path);

/**
 * Length of @p name without its program suffix, for showing it.
 *
 * @param name A file name, not a path; pass it through appname_base() first.
 */
size_t appname_length(const char *name);

/** Whether @p a and @p b name the same program. */
bool appname_same(const char *a, const char *b);

#endif /* APPNAME_H */
