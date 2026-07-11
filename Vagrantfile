Vagrant.configure("2") do |config|
  config.vm.define "minicam-hal-lab" do |vm|
    # ARM64 Ubuntu guest image for C++ camera pipeline, Linux V4L2, and later
    # Android-style HAL experiments.
    vm.vm.box = "perk/ubuntu-25.04-arm64"
    vm.vm.box_version = "20250424"
    vm.vm.box_architecture = "arm64"

    # Host-side provider. This boots the VM with QEMU on the host machine.
    vm.vm.provider "qemu" do |qemu|
      qemu.ssh_port = "50024"
      qemu.memory = "4096"
      qemu.cpus = 2
    end

    # Keep source on the host and sync it into the guest. Build outputs stay out
    # of the shared source tree.
    vm.vm.synced_folder ".", "/home/vagrant/minicam-hal",
      type: "rsync",
      rsync__args: ["--verbose", "--archive", "--delete", "-z",
                    "--exclude", "build*/", "--exclude", "artifacts/"],
      rsync__exclude: ["build*/", "artifacts/", ".vagrant/", ".DS_Store"],
      rsync__auto: true

    vm.vm.provision "shell", inline: <<-SHELL
      set -eux

      sudo apt-get update

      sudo apt-get install -y \
        ca-certificates apt-transport-https software-properties-common \
        build-essential git curl wget vim htop unzip plocate gnupg lsb-release \
        pkg-config cmake ninja-build meson clang llvm lldb gdb valgrind \
        python3 python3-pip python3-venv \
        qemu-system qemu-utils \
        zlib1g-dev libelf-dev libzstd-dev libseccomp-dev \
        linux-headers-$(uname -r) linux-tools-$(uname -r) linux-tools-common \
        linux-modules-extra-$(uname -r) \
        v4l-utils bsdutils strace ltrace systemtap-sdt-dev protobuf-compiler

      if modinfo vivid >/dev/null 2>&1; then
        sudo modprobe vivid
        sudo usermod -aG video vagrant
        sudo tee /etc/modules-load.d/minicam-vivid.conf >/dev/null <<'EOF'
vivid
EOF
        v4l2-ctl --list-devices || true
      else
        echo "vivid kernel module is not available for kernel $(uname -r); V4L2 capture tests can use a real USB camera or another kernel with vivid enabled."
      fi

      sudo -u vagrant mkdir -p /home/vagrant/workspace

      grep -qxF 'export MINICAM_WORKSPACE=$HOME/minicam-hal' /home/vagrant/.bashrc || echo 'export MINICAM_WORKSPACE=$HOME/minicam-hal' >> /home/vagrant/.bashrc

      cd /tmp

      GO_VERSION="1.24.2"
      if [ ! -x /usr/local/go/bin/go ]; then
        wget -q https://go.dev/dl/go${GO_VERSION}.linux-arm64.tar.gz
        sudo rm -rf /usr/local/go
        sudo tar -C /usr/local -xzf go${GO_VERSION}.linux-arm64.tar.gz
        rm go${GO_VERSION}.linux-arm64.tar.gz
      fi
      grep -qxF 'export PATH=$PATH:/usr/local/go/bin' /home/vagrant/.bashrc || echo 'export PATH=$PATH:/usr/local/go/bin' >> /home/vagrant/.bashrc
      grep -qxF 'export GOPATH=$HOME/go' /home/vagrant/.bashrc || echo 'export GOPATH=$HOME/go' >> /home/vagrant/.bashrc
      grep -qxF 'export PATH=$PATH:$GOPATH/bin' /home/vagrant/.bashrc || echo 'export PATH=$PATH:$GOPATH/bin' >> /home/vagrant/.bashrc

      sudo mkdir -p /etc/apt/keyrings
      if [ ! -f /etc/apt/keyrings/docker.gpg ]; then
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
      fi
      echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
      sudo apt-get update
      sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
      sudo usermod -aG docker vagrant
    SHELL
  end
end
