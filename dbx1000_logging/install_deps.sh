apt-get update
apt-get install -y --force-yes --no-install-recommends apt-utils
apt-get install -y --force-yes --no-install-recommends python3
apt-get install -y --force-yes --no-install-recommends wget
apt-get install -y --force-yes --no-install-recommends numactl
#apt-get install -y --force-yes --no-install-recommends openjdk-14-jdk
apt-get install -y --force-yes --no-install-recommends libjemalloc-dev
apt-get install -y --force-yes --no-install-recommends libntl-dev
# on some systems we need to install add-apt-repository 
apt-get install -y --force-yes --no-install-recommends apt-transport-https software-properties-common gnupg
apt-get update

wget -qO - https://adoptopenjdk.jfrog.io/adoptopenjdk/api/gpg/key/public | apt-key add -
add-apt-repository --yes https://adoptopenjdk.jfrog.io/adoptopenjdk/deb/

echo "Installing OpenJDK 14"
apt-get update
apt-get install -y --force-yes --no-install-recommends adoptopenjdk-14-hotspot

