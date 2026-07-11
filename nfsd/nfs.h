/*
 * nfs.h - program/version numbers and the export configuration for
 * the simple NFSv2 server (gnfsd).
 */
#ifndef GN_NFS_H
#define GN_NFS_H

#include "rpc.h"

#define PMAP_PROG 100000
#define PMAP_VERS 2
#define MOUNT_PROG 100005
#define MOUNT_VERS 1
#define NFS_PROG  100003
#define NFS_VERS  2

#define IPPROTO_UDP_ 17

/* set by main: the exported directory (absolute, no trailing
   slash) and the UDP port where mount+nfs are served (portmapper
   GETPORT reports this; many old clients also assume nfs=2049). */
void svc_config(const char *export_root, int nfs_port);

/* route tables (terminated by counts passed to rpc_serve) */
extern const rpc_route_t GN_ROUTES[];
extern const int GN_NROUTES;

#endif /* GN_NFS_H */
