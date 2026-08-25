# SPDX-License-Identifier: Apache-2.0

terraform {
  required_version = "= 1.12.3"

  required_providers {
    libvirt = {
      source  = "dmacvicar/libvirt"
      version = "= 0.9.8"
    }
  }
}

provider "libvirt" {
  uri = var.libvirt_uri
}
