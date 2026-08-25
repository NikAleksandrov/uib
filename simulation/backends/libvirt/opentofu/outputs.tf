# SPDX-License-Identifier: Apache-2.0

output "nodes" {
  value = {
    for name, node in var.nodes : name => {
      domain        = libvirt_domain.node[name].name
      role          = node.role
      management_ip = node.management_ip
      data_ip       = node.data_ip
    }
  }
}

output "networks" {
  value = {
    management = var.network_mode == "bridge" ? libvirt_network.management[0].name : "passt"
    data       = var.network_mode == "bridge" ? libvirt_network.data[0].name : "mcast://${var.multicast_address}:${var.multicast_port}"
  }
}
