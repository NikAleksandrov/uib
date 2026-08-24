# SPDX-License-Identifier: Apache-2.0

def render(config, run_id, allow_mutable_tags):
    from .inference import render_inference
    from .training import render_training

    kind = config.get("workload", {}).get("kind")
    if kind == "inference":
        render_inference(config, run_id, allow_mutable_tags)
    else:
        render_training(config, run_id, allow_mutable_tags)
