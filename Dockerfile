# Dockerfile for a quickstart 

FROM ubuntu:20.04

LABEL maintainer="ssaleki@ncsu.edu"

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get clean \
  && apt-get install -y --no-install-recommends ssh \
      build-essential \
      gcc \
      g++ \
      gdb \
      cmake \
      rsync \
      tar \
      python3 \
      python-is-python3\
      pip \
      zlib1g-dev \
      bison \
      libbison-dev \
      flex \
   && apt-get clean

RUN apt-get clean \
    && apt-get install -y --no-install-recommends libllvm-8-ocaml-dev libllvm8 llvm-8 llvm-8-dev llvm-8-doc llvm-8-examples llvm-8-runtime clang-8 clang-tools-8 clang-8-doc libclang-common-8-dev libclang-8-dev libclang1-8 clang-format-8 python3-clang-8 clangd-8 libfuzzer-8-dev lldb-8 lld-8 libc++-8-dev libc++abi-8-dev libomp-8-dev libclc-12-dev libunwind-dev libfl-dev \
    && apt-get clean

RUN apt-get autoclean
RUN apt-get autoremove

RUN apt-get install -y time \
    git \
    vim \
    && apt-get clean

RUN apt install -y python3-pip
RUN pip3 install lit

ADD . /llvmpass
ADD . /build
WORKDIR /llvmpass

RUN ( \
    echo 'LogLevel DEBUG2'; \
    echo 'PermitRootLogin yes'; \
    echo 'PasswordAuthentication yes'; \
    echo 'Subsystem sftp /usr/lib/openssh/sftp-server'; \
  ) > /etc/ssh/sshd_config_test_clion \
  && mkdir /run/sshd

RUN useradd -m user \
  && yes password | passwd user

COPY . /llvmpass
COPY .vimrc /root/.vimrc
CMD ["/usr/sbin/sshd", "-D", "-e", "-f", "/etc/ssh/sshd_config_test_clion"]
