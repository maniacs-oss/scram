#!/usr/bin/env bash

set -ev

if [[ "${TRAVIS_OS_NAME}" == "osx" ]]; then
  # brew update
  # brew outdated boost || brew upgrade boost
  brew install libxml++
  brew install gperftools
  brew install qt5
  brew install qwt
  brew install ccache
fi

sudo -H pip install -U pip wheel
sudo -H pip install nose  # Testing main() requires nosetests!

[[ "${TRAVIS_OS_NAME}" == "linux" ]] || exit 0

if [[ -n "${INTEL_COMPILER}" ]]; then
  git clone --branch icc17 https://github.com/rakhimov/icc-travis.git
  ./icc-travis/install-icc.sh
fi

if [[ "$CXX" = "clang++" ]]; then
  sudo apt-get install -qq clang-${CLANG_VERSION}
fi
# Boost
sudo apt-get install \
  libboost-{program-options,math,random,filesystem,system,date-time}1.58-dev

# Installing dependencies from source.
PROJECT_DIR=$PWD
cd /tmp

# Libxml++
LIBXMLPP='libxml++-2.38.1'
wget http://ftp.gnome.org/pub/GNOME/sources/libxml++/2.38/${LIBXMLPP}.tar.xz
tar -xf ${LIBXMLPP}.tar.xz
(cd ${LIBXMLPP} && ./configure && make -j 2 && sudo make install)

cd $PROJECT_DIR

[[ -z "${RELEASE}" && "$CXX" = "g++" ]] || exit 0

# Install newer doxygen due to bugs in 1.8.6 with C++11 code.
DOXYGEN='doxygen-1.8.11.linux.bin.tar.gz'
wget https://downloads.sourceforge.net/project/doxygen/rel-1.8.11/${DOXYGEN}
tar -xf ${DOXYGEN}
sudo cp doxygen-1.8.11/bin/* /usr/bin/

sudo apt-get install -qq ggcov
sudo apt-get install -qq valgrind
sudo apt-get install -qq lcov

sudo -H pip install -r requirements-dev.txt
sudo -H pip install -r requirements-tests.txt
