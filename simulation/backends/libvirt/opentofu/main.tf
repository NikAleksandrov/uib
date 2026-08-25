# SPDX-License-Identifier: Apache-2.0

locals {
  management_gateway = cidrhost(var.management_cidr, 1)
  management_prefix  = tonumber(split("/", var.management_cidr)[1])
  data_gateway       = cidrhost(var.data_cidr, 1)
  data_prefix        = tonumber(split("/", var.data_cidr)[1])
}

resource "libvirt_network" "management" {
  count = var.network_mode == "bridge" ? 1 : 0

  name = "${var.run_id}-mgmt"

  forward = {
    mode = "nat"
  }
  mtu = {
    size = var.mtu
  }
  ips = [{
    address = local.management_gateway
    prefix  = local.management_prefix
    dhcp = {
      hosts = [for name, node in var.nodes : {
        name = name
        ip   = node.management_ip
        mac  = node.management_mac
      }]
    }
  }]
  metadata = {
    xml = "<uib:run xmlns:uib=\"https://uib.example/libvirt\" id=\"${var.run_id}\" layer=\"management\"/>"
  }
}

resource "libvirt_network" "data" {
  count = var.network_mode == "bridge" ? 1 : 0

  name = "${var.run_id}-data"

  mtu = {
    size = var.mtu
  }
  ips = [{
    address = local.data_gateway
    prefix  = local.data_prefix
  }]
  metadata = {
    xml = "<uib:run xmlns:uib=\"https://uib.example/libvirt\" id=\"${var.run_id}\" layer=\"data\"/>"
  }
}

resource "libvirt_volume" "base" {
  name = "${var.run_id}-base.qcow2"
  pool = var.pool
  target = {
    format = {
      type = "qcow2"
    }
  }
  create = {
    content = {
      url = var.base_image
    }
  }
}

resource "libvirt_volume" "disk" {
  for_each = var.nodes

  name     = "${var.run_id}-${each.key}.qcow2"
  pool     = var.pool
  capacity = each.value.disk_bytes
  target = {
    format = {
      type = "qcow2"
    }
  }
  backing_store = {
    path = libvirt_volume.base.path
    format = {
      type = "qcow2"
    }
  }
}

resource "libvirt_cloudinit_disk" "node" {
  for_each = var.nodes

  name = "${var.run_id}-${each.key}-cloudinit"
  meta_data = yamlencode({
    instance-id    = "${var.run_id}-${each.key}"
    local-hostname = each.key
  })
  user_data = "#cloud-config\n${yamlencode({
    users = [{
      name                = var.ssh_user
      groups              = ["adm", "sudo"]
      lock_passwd         = true
      shell               = "/bin/bash"
      ssh_authorized_keys = [var.ssh_public_key]
      sudo                = "ALL=(ALL) NOPASSWD:ALL"
    }]
    disable_root   = true
    package_update = true
    packages       = ["python3", "qemu-guest-agent"]
    ssh_pwauth     = false
    timezone       = "UTC"
    runcmd         = [["systemctl", "enable", "--now", "qemu-guest-agent"]]
  })}"
  network_config = yamlencode({
    version = 2
    ethernets = {
      management = {
        match = {
          macaddress = each.value.management_mac
        }
        set-name  = "eth0"
        dhcp4     = var.network_mode == "rootless"
        addresses = var.network_mode == "bridge" ? ["${each.value.management_ip}/${local.management_prefix}"] : []
        routes = var.network_mode == "bridge" ? [{
          to  = "default"
          via = local.management_gateway
        }] : []
        nameservers = {
          addresses = var.network_mode == "bridge" ? [local.management_gateway] : []
        }
      }
      data = {
        match = {
          macaddress = each.value.data_mac
        }
        set-name  = "eth1"
        addresses = ["${each.value.data_ip}/${local.data_prefix}"]
      }
    }
  })
}

resource "libvirt_volume" "cloudinit" {
  for_each = var.nodes

  name = "${var.run_id}-${each.key}-cloudinit.iso"
  pool = var.pool
  create = {
    content = {
      url = libvirt_cloudinit_disk.node[each.key].path
    }
  }
}

resource "libvirt_domain" "node" {
  for_each = var.nodes

  name        = "${var.run_id}-${each.key}"
  memory      = each.value.memory_mib
  memory_unit = "MiB"
  vcpu        = each.value.cpu
  type        = "kvm"
  running     = true

  metadata = {
    xml = "<uib:run xmlns:uib=\"https://uib.example/libvirt\" id=\"${var.run_id}\" role=\"${each.value.role}\"/>"
  }
  cpu = {
    mode = "host-passthrough"
  }
  os = {
    type         = "hvm"
    type_arch    = "x86_64"
    type_machine = "q35"
    boot_devices = [{ dev = "hd" }]
  }
  devices = {
    disks = [
      {
        source = {
          volume = {
            pool   = libvirt_volume.disk[each.key].pool
            volume = libvirt_volume.disk[each.key].name
          }
        }
        target = {
          dev = "vda"
          bus = "virtio"
        }
        driver = {
          type = "qcow2"
        }
      },
      {
        device = "cdrom"
        source = {
          volume = {
            pool   = libvirt_volume.cloudinit[each.key].pool
            volume = libvirt_volume.cloudinit[each.key].name
          }
        }
        target = {
          dev = "sda"
          bus = "sata"
        }
      }
    ]
    interfaces = [
      {
        mac = {
          address = each.value.management_mac
        }
        backend = var.network_mode == "rootless" ? {
          type = "passt"
        } : null
        model = {
          type = "virtio"
        }
        source = {
          network = var.network_mode == "bridge" ? {
            network = libvirt_network.management[0].name
          } : null
          user = var.network_mode == "rootless" ? {} : null
        }
        port_forward = var.network_mode == "rootless" ? [{
          proto   = "tcp"
          address = "127.0.0.1"
          ranges = [{
            start = each.value.ssh_port
            to    = 22
          }]
        }] : null
        wait_for_ip = var.network_mode == "bridge" ? {
          source  = "agent"
          timeout = 300
        } : null
      },
      {
        mac = {
          address = each.value.data_mac
        }
        model = {
          type = "virtio"
        }
        source = {
          network = var.network_mode == "bridge" ? {
            network = libvirt_network.data[0].name
          } : null
          mcast = var.network_mode == "rootless" ? {
            address = var.multicast_address
            port    = var.multicast_port
          } : null
        }
      }
    ]
    channels = [{
      source = {
        unix = {
          mode = "bind"
        }
      }
      target = {
        virt_io = {
          name = "org.qemu.guest_agent.0"
        }
      }
    }]
    graphics = [{
      vnc = {
        auto_port = true
        listen    = "127.0.0.1"
      }
    }]
  }

  depends_on = [libvirt_volume.cloudinit]
}
