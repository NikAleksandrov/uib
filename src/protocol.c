/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>

#include "uib/protocol.h"
#include "uib/env.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const struct uib_protocol_ops *const protocols[] = {
	&uib_rocev2_protocol_ops,
	&uib_dummy_protocol_ops,
	&uib_uet_protocol_ops,
};

/**
 * uib_protocol_find - Find registered protocol operations by name.
 * @name: Wire protocol name
 * @uib-api: Protocol selection
 *
 * Return: The matching protocol operations, the RoCEv2 operations for an empty
 * name, or NULL for an unknown name.
 */
const struct uib_protocol_ops *uib_protocol_find(const char *name)
{
	size_t i;

	if (!name || !*name)
		return &uib_rocev2_protocol_ops;

	for (i = 0; i < ARRAY_SIZE(protocols); i++) {
		if (!strcmp(name, protocols[i]->name))
			return protocols[i];
	}

	return NULL;
}

/**
 * uib_protocol_from_env - Select protocol operations from UIB_PROTOCOL.
 * @uib-api: Protocol selection
 *
 * Return: The selected protocol operations, or NULL if UIB_PROTOCOL is
 * invalid.
 */
const struct uib_protocol_ops *uib_protocol_from_env(void)
{
	return uib_protocol_find(getenv(UIB_ENV_PROTOCOL));
}

/**
 * uib_protocol_default_port - Return a protocol's standard service port.
 * @name: Wire protocol name
 * @uib-api: Protocol selection
 *
 * Return: The protocol's standard port in host byte order, or 0 if @name is
 * unknown.
 */
uint16_t uib_protocol_default_port(const char *name)
{
	const struct uib_protocol_ops *ops = uib_protocol_find(name);

	return ops ? ops->default_port : 0;
}
