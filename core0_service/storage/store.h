/*
 * store.h - optional local storage for events / gate log (P4-07).
 *
 * Compiled out unless ENABLE_STORAGE is defined (default off, spec
 * 5.7.1). When enabled, records are appended to <dir>/events.log and
 * <dir>/gate_log by a dedicated low-frequency async writer thread:
 * callers never block on disk I/O and only state changes produce a
 * record (eMMC friendly, spec 5.7.2). Write failures are logged as
 * ERROR and never affect the business flow (spec 5.7.3).
 *
 * The directory must exist; it is not created implicitly.
 * Portable (pthread), also links into the host selftest.
 */
#ifndef BIZ_STORE_H
#define BIZ_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* returns 0 when ready (or compiled out), -1 when enable requested
 * but the writer thread could not start */
int  store_init(const char *dir);

/* event log: ts, type, plate, detail */
void store_event(const char *type, const char *plate, const char *detail);
/* gate log: ts, action ("open"/"close"), source ("auto"/"ui"/"state") */
void store_gate(const char *action, const char *source);

void store_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BIZ_STORE_H */
