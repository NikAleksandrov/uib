# SPDX-License-Identifier: Apache-2.0

variable "run_id" {
  type = string

  validation {
    condition     = can(regex("^[a-z][a-z0-9-]{2,47}$", var.run_id))
    error_message = "run_id must be a 3-48 character lowercase DNS label"
  }
}

variable "libvirt_uri" {
  type = string
}

variable "pool" {
  type = string
}

variable "base_image" {
  type = string
}

variable "volume_owner" {
  type = string
}

variable "volume_group" {
  type = string
}

variable "ssh_user" {
  type = string
}

variable "ssh_public_key" {
  type      = string
  sensitive = true
}

variable "network_mode" {
  type = string

  validation {
    condition     = contains(["bridge", "rootless"], var.network_mode)
    error_message = "network_mode must be bridge or rootless"
  }
}

variable "management_cidr" {
  type = string
}

variable "data_cidr" {
  type = string
}

variable "mtu" {
  type = number
}

variable "multicast_address" {
  type = string
}

variable "multicast_port" {
  type = number
}

variable "nodes" {
  type = map(object({
    role           = string
    index          = number
    cpu            = number
    memory_mib     = number
    disk_bytes     = number
    management_ip  = string
    management_mac = string
    ssh_port       = number
    data_ip        = string
    data_mac       = string
  }))
}
