/*
 * acl.h - dotted-quad IPv4 allow-list matching (PLAN_00 D4).
 * No socket headers: a self-contained parser so it links in every
 * build. Used by the TCP daemon to gate incoming peers.
 */
#ifndef ACL_H
#define ACL_H

/* Is `ip` (dotted quad "a.b.c.d") permitted by `allow`?
   `allow` is a space/comma separated list of entries, each either
   "a.b.c.d" (exact) or "a.b.c.d/bits" (CIDR). An empty or NULL
   `allow` permits everyone (default). A malformed entry never
   matches. Returns 1 if allowed, 0 if denied. */
int acl_allowed(const char *allow, const char *ip);

#endif /* ACL_H */
