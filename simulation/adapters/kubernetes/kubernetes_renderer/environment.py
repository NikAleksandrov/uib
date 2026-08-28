# SPDX-License-Identifier: Apache-2.0

def environment(config):
    from .core import RenderError, active_mtu, string_value

    uib = config["uib"]
    communication = config["communication"]
    frontend = communication["frontend"]
    collective = communication[frontend]
    values = {
        "NCCL_DEBUG": "INFO",
        "NCCL_DEBUG_SUBSYS": "INIT,NET",
        "NCCL_IB_DISABLE": "0",
        "NCCL_IB_HCA": collective["hca"],
        "NCCL_NET": collective["network"],
        "HF_HOME": "/cache/huggingface",
    }
    if config["storage"].get("modelCacheImage"):
        values["HF_HUB_OFFLINE"] = "1"
        values["TRANSFORMERS_OFFLINE"] = "1"
    protected_nccl = {
        "NCCL_IBVERBS_LIB",
        "NCCL_IB_DISABLE",
        "NCCL_IB_HCA",
        "NCCL_NET",
    }
    for name, value in collective.get("environment", {}).items():
        if name in protected_nccl:
            raise RenderError(f"{name} is controlled by the resolved communication layer")
        values[name] = string_value(value)

    value_from = {}
    if uib["enabled"]:
        values.update(
            {
                "LD_LIBRARY_PATH": "/opt/uib/lib",
                "NCCL_IBVERBS_LIB": "/opt/uib/lib/libuibv.so",
                "UIB_ACTIVE_MTU": string_value(
                    active_mtu(config["network"]["mtu"])
                ),
                "UIB_BIND_PORT": string_value(uib["bindPort"]),
                "UIB_PROTOCOL": uib["protocol"],
                "UIB_STATS_OUTPUT": "stderr",
                "UIB_TRANSPORT_MODE": uib["transport"],
                "UIB_ZEROCOPY": "off",
            }
        )
        value_from["UIB_BIND_IP"] = {
            "fieldRef": {"fieldPath": "status.podIP"}
        }
        cc = uib["congestionControl"]
        cc_prefix = ("UIB_UET" if uib["protocol"] == "uet" else
                     "UIB_ROCEV2")
        values[f"{cc_prefix}_CC"] = cc["algorithm"]
        parameter_environment = {
            "dcqcn": {
                "initialRateBytesPerSecond": "UIB_DCQCN_INITIAL_RATE",
                "minimumRateBytesPerSecond": "UIB_DCQCN_MINIMUM_RATE",
                "maximumRateBytesPerSecond": "UIB_DCQCN_MAXIMUM_RATE",
                "additiveIncreaseBytesPerSecond":
                    "UIB_DCQCN_ADDITIVE_INCREASE",
                "recoveryIntervalNs": "UIB_DCQCN_RECOVERY_INTERVAL",
                "alphaGainQ16": "UIB_DCQCN_ALPHA_GAIN",
            },
            "rccc": {
                "maxWindowBytes": "UIB_UET_RCCC_MAX_WND",
                "creditQuantumBytes": "UIB_UET_RCCC_CREDIT_QUANTUM",
                "linkRateBytesPerSecond": "UIB_UET_RCCC_LINK_RATE",
                "baseRttNs": "UIB_UET_RCCC_BASE_RTT",
            },
        }
        if cc["algorithm"] == "nscc":
            parameter_environment["nscc"] = {
                "maxWindowBytes": f"{cc_prefix}_NSCC_MAX_WND"
            }
        for name, value in cc["parameters"].items():
            environment_name = parameter_environment[cc["algorithm"]][name]
            values[environment_name] = string_value(value)

        protected_uib = {
            "UIB_ACTIVE_MTU",
            "UIB_BIND_IP",
            "UIB_BIND_PORT",
            "UIB_PROTOCOL",
            "UIB_STATS_OUTPUT",
            "UIB_TRANSPORT_MODE",
            "UIB_UET_CC",
            "UIB_UET_NSCC_MAX_WND",
            "UIB_UET_RCCC_MAX_WND",
            "UIB_UET_RCCC_CREDIT_QUANTUM",
            "UIB_UET_RCCC_LINK_RATE",
            "UIB_UET_RCCC_BASE_RTT",
            "UIB_ROCEV2_CC",
            "UIB_ROCEV2_NSCC_MAX_WND",
            "UIB_DCQCN_INITIAL_RATE",
            "UIB_DCQCN_MINIMUM_RATE",
            "UIB_DCQCN_MAXIMUM_RATE",
            "UIB_DCQCN_ADDITIVE_INCREASE",
            "UIB_DCQCN_RECOVERY_INTERVAL",
            "UIB_DCQCN_ALPHA_GAIN",
            "UIB_ZEROCOPY",
        }
        for name, value in uib.get("environment", {}).items():
            if name in protected_uib:
                raise RenderError(f"{name} is controlled by the resolved UIB layer")
            values[name] = string_value(value)

    result = [{"name": name, "value": value} for name, value in sorted(values.items())]
    result.extend(
        {"name": name, "valueFrom": value}
        for name, value in sorted(value_from.items())
    )
    return result
