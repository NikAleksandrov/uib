# SPDX-License-Identifier: Apache-2.0

def inventory(topology_value, key, manager):
    hosts = {}
    for name, guest in topology_value["guests"].items():
        hosts[name] = {
            "ansible_host": "127.0.0.1",
            "ansible_port": guest["sshPort"],
            "uib_data_ip": guest["dataAddress"],
            "uib_role": guest["role"],
        }
    controller = {name: {} for name, guest in
                  topology_value["guests"].items()
                  if guest["role"] == "controller"}
    workers = {name: {} for name, guest in topology_value["guests"].items()
               if guest["role"] == "worker"}
    return {
        "all": {
            "vars": {
                "ansible_user": "root",
                "ansible_ssh_private_key_file": str(key),
                "ansible_ssh_common_args": (
                    "-o IdentitiesOnly=yes -o StrictHostKeyChecking=no "
                    "-o UserKnownHostsFile=/dev/null"),
            },
            "children": {
                f"{manager}_controller": {"hosts": controller},
                f"{manager}_workers": {"hosts": workers},
                f"{manager}_cluster": {"hosts": hosts},
            },
        },
    }
