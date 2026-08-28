/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_CONGESTION_INTERNAL_H
#define UIB_CONGESTION_INTERNAL_H

#include <stdint.h>

struct uib_dcqcn_config;
struct uib_uet_rccc_config;

int uib_dcqcn_config_from_env(struct uib_dcqcn_config *config, uint32_t mtu);
int uib_uet_rccc_config_from_env(struct uib_uet_rccc_config *config,
				 uint32_t mtu);

#endif
