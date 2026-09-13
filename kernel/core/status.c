// SPDX-License-Identifier: MIT
// AfriyieOS — status code names

#include "afriyie/status.h"

const char *af_status_name(af_status_t status)
{
    switch (status) {
    // --- success ---
    case AF_OK:              return "OK";

    // --- generic ---
    case AF_ERR_GENERIC:     return "ERR_GENERIC";
    case AF_ERR_INVAL:       return "ERR_INVAL";
    case AF_ERR_NOMEM:       return "ERR_NOMEM";
    case AF_ERR_NOENT:       return "ERR_NOENT";
    case AF_ERR_EXIST:       return "ERR_EXIST";
    case AF_ERR_BUSY:        return "ERR_BUSY";
    case AF_ERR_AGAIN:       return "ERR_AGAIN";
    case AF_ERR_NOTSUP:      return "ERR_NOTSUP";
    case AF_ERR_FAULT:       return "ERR_FAULT";
    case AF_ERR_OVERFLOW:    return "ERR_OVERFLOW";
    case AF_ERR_TIMEOUT:     return "ERR_TIMEOUT";
    case AF_ERR_CANCELED:    return "ERR_CANCELED";

    // --- capability / security ---
    case AF_ERR_PERM:        return "ERR_PERM";
    case AF_ERR_CAP_INVALID: return "ERR_CAP_INVALID";
    case AF_ERR_CAP_EXHAUST: return "ERR_CAP_EXHAUST";
    case AF_ERR_PEER_DEAD:   return "ERR_PEER_DEAD";

    // --- object / process ---
    case AF_ERR_NOOBJ:       return "ERR_NOOBJ";
    case AF_ERR_TOOMANY:     return "ERR_TOOMANY";
    case AF_ERR_NOTREADY:    return "ERR_NOTREADY";
    case AF_ERR_AFFINITY:    return "ERR_AFFINITY";

    // --- boot / firmware ---
    case AF_ERR_BOOT_MAGIC:   return "ERR_BOOT_MAGIC";
    case AF_ERR_BOOT_MEMMAP:  return "ERR_BOOT_MEMMAP";
    case AF_ERR_BOOT_NOFB:    return "ERR_BOOT_NOFB";
    case AF_ERR_BOOT_VERSION: return "ERR_BOOT_VERSION";

    // --- storage / file system ---
    case AF_ERR_IO:          return "ERR_IO";
    case AF_ERR_NODEV:       return "ERR_NODEV";
    case AF_ERR_FS_CORRUPT:  return "ERR_FS_CORRUPT";
    case AF_ERR_NOTDIR:      return "ERR_NOTDIR";
    case AF_ERR_ISDIR:       return "ERR_ISDIR";
    case AF_ERR_NOSPC:       return "ERR_NOSPC";
    case AF_ERR_ROFS:        return "ERR_ROFS";

    default:                 return "ERR_UNKNOWN";
    }
}
