/* Cloud accounts (OneDrive, Dropbox). */
#include "store.h"

bd_store *bd_cloud_store_open(bd_catalog *cat, int64_t media_id)
{
    (void)media_id;
    bd_fail(cat, BD_ERR_NOT_FOUND, "cloud storage is not available in this build");
    return NULL;
}
