/*
 * Dandelion (Redmi 9A / MT6762G) platform definitions.
 *
 * The real content lives in the XNU tree layout so the directory can be
 * overlaid onto an XNU source tree unchanged:
 *
 *     pexpert/pexpert/arm64/DANDELION.h
 *
 * XNU's board_config.h picks it up via ARM64_BOARD_CONFIG_DANDELION.
 * This forwarding header keeps the freestanding compile check and the
 * platform expert working with -Ipexpert.
 */
#ifndef __PE_DANDELION_H__
#define __PE_DANDELION_H__

#include "pexpert/arm64/DANDELION.h"

#endif /* __PE_DANDELION_H__ */
