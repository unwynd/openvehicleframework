# SPDX-License-Identifier: Apache-2.0

variable "OVF_LAB_TAG" {
  default = "openvehicleframework/dev-lab:local"
}

group "default" {
  targets = ["lab"]
}

target "lab" {
  context    = "."
  dockerfile = "lab/Dockerfile"
  tags       = ["${OVF_LAB_TAG}"]
  platforms  = ["linux/amd64", "linux/arm64"]
}

target "lab-amd64" {
  inherits = ["lab"]
  platforms = ["linux/amd64"]
}

target "lab-arm64" {
  inherits = ["lab"]
  platforms = ["linux/arm64"]
}

target "validated-amd64" {
  inherits  = ["lab-amd64"]
  args      = { OVF_PREBUILD = "1" }
}

target "validated-arm64" {
  inherits  = ["lab-arm64"]
  args      = { OVF_PREBUILD = "1" }
}

target "rootfs-amd64" {
  inherits  = ["validated-amd64"]
  platforms = ["linux/amd64"]
  output    = ["type=local,dest=out/linux-rootfs-amd64"]
}

target "rootfs-arm64" {
  inherits  = ["validated-arm64"]
  platforms = ["linux/arm64"]
  output    = ["type=local,dest=out/linux-rootfs-arm64"]
}
