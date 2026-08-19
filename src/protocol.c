#include <stdlib.h>
#include <string.h>

#include "uib/protocol.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const struct uib_protocol_ops *const protocols[] = {
	&uib_rocev2_protocol_ops,
	&uib_dummy_protocol_ops,
	&uib_uet_protocol_ops,
};

const struct uib_protocol_ops *uib_protocol_find(const char *name)
{
	if (!name || !*name)
		return &uib_rocev2_protocol_ops;

	for (size_t i = 0; i < ARRAY_SIZE(protocols); i++) {
		if (!strcmp(name, protocols[i]->name))
			return protocols[i];
	}

	return NULL;
}

const struct uib_protocol_ops *uib_protocol_from_env(void)
{
	return uib_protocol_find(getenv("UIB_PROTOCOL"));
}

uint16_t uib_protocol_default_port(const char *name)
{
	const struct uib_protocol_ops *ops = uib_protocol_find(name);

	return ops ? ops->default_port : 0;
}
