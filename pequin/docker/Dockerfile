FROM debian:jessie

ENV PEQUIN /opt/pequin
ENV LITMUS /opt/litmus

RUN set -ex \
    && buildDeps=" \
    python2.7 libpython2.7 openjdk-7-jdk \
    golang build-essential gcc-6 g++-6 gcc-6-multilib g++-6-multilib ant  \
    ant-optional make time libboost-all-dev libgmp10 libgmp-dev   \
    zlib1g zlib1g-dev libssl-dev cmake git pkg-config \
    " \
    && echo "deb http://ftp.de.debian.org/debian stretch main" >> /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends $buildDeps

COPY ../dbx1000_logging $LITMUS

COPY . $PEQUIN
RUN cd $PEQUIN/thirdparty && ./install_pepper_deps.sh \
#    && cd $PEQUIN && ./install_buffet.sh \
#    && mv $PEQUIN/thirdparty/libsnark /tmp && rm -rf $PEQUIN/thirdparty/* \
#    && mv /tmp/libsnark $PEQUIN/thirdparty/ \
#    && rm -rf $PEQUIN/compiler/buffetfsm/llvm $PEQUIN/compiler/buffetfsm/llvm-build
CMD ["/bin/bash"]