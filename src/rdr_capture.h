#ifndef RDR_CAPTURE_H
#define RDR_CAPTURE_H
#include "rdr.h"
rbackend_t *rdr_capture_wrap(rbackend_t *inner, const char *path);
#endif
