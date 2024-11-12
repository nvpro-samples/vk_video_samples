#!/bin/bash
# Update source for glslang

set -e

if [[ $(uname) == "Linux" || $(uname) =~ "CYGWIN" ]]; then
    CURRENT_DIR="$(dirname "$(readlink -f ${BASH_SOURCE[0]})")"
    CORE_COUNT=$(nproc || echo 4)
elif [[ $(uname) == "Darwin" ]]; then
    CURRENT_DIR="$(dirname "$(python -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' ${BASH_SOURCE[0]})")"
    CORE_COUNT=$(sysctl -n hw.ncpu || echo 4)
fi
echo CURRENT_DIR=$CURRENT_DIR
echo CORE_COUNT=$CORE_COUNT

REVISION_DIR="$CURRENT_DIR/vk_video_decoder/external_revisions"

# git submodule update --init --recursive

# Use tr -d to remove line endings
GLSLANG_GITURL=$(cat "${REVISION_DIR}/glslang_giturl" | tr -d "\n\r")
GLSLANG_REVISION=$(cat "${REVISION_DIR}/glslang_revision" | tr -d "\n\r")
SHADERC_GITURL=$(cat "${REVISION_DIR}/shaderc_giturl" | tr -d "\n\r")
SHADERC_REVISION=$(cat "${REVISION_DIR}/shaderc_revision" | tr -d "\n\r")

echo "GLSLANG_GITURL=${GLSLANG_GITURL}"
echo "GLSLANG_REVISION=${GLSLANG_REVISION}"

echo "SHADERC_GITURL=${SHADERC_GITURL}"
echo "SHADERC_REVISION=${SHADERC_REVISION}"

BUILDDIR=${CURRENT_DIR}
BASEDIR="$BUILDDIR/vk_video_decoder/external"

function create_glslang () {
   rm -rf "${BASEDIR}"/glslang
   echo "Creating local glslang repository (${BASEDIR}/glslang)."
   mkdir -p "${BASEDIR}"/glslang
   cd "${BASEDIR}"/glslang
   git clone ${GLSLANG_GITURL} .
   git checkout ${GLSLANG_REVISION}
   ./update_glslang_sources.py
}

function update_glslang () {
   echo "Updating ${BASEDIR}/glslang"
   cd "${BASEDIR}"/glslang
   git fetch origin
   git checkout --force ${GLSLANG_REVISION}
   ./update_glslang_sources.py
}

function build_glslang () {
   echo "Building ${BASEDIR}/glslang"
   cd "${BASEDIR}"/glslang
   mkdir -p build
   cd build
   cmake -D CMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=install ..
   make -j $CORE_COUNT
   make install
}

function create_shaderc () {
   rm -rf "${BASEDIR}"/shaderc
   echo "Creating local shaderc repository (${BASEDIR}/shaderc)."
   mkdir -p "${BASEDIR}"/shaderc
   cd "${BASEDIR}"/shaderc
   git clone ${SHADERC_GITURL} .
   git checkout ${SHADERC_REVISION}
#   ./update_shaderc_sources.py
}

function update_shaderc () {
   echo "Updating ${BASEDIR}/shaderc"
   cd "${BASEDIR}"/shaderc
   git fetch origin
   git checkout --force ${SHADERC_REVISION}
   ./utils/git-sync-deps
}

# glslang/External/spirv-tools/external/spirv-headers
function build_shaderc () {
   echo "Building ${BASEDIR}/shaderc"
   cd "${BASEDIR}"/shaderc
   mkdir -p build
   cd build
   cmake -DSHADERC_SKIP_TESTS=ON -DSHADERC_SPIRV_TOOLS_DIR=${BASEDIR}/glslang/External/spirv-tools -DSHADERC_SPIRV_HEADERS_DIR=${BASEDIR}/glslang/External/spirv-tools/external/spirv-headers -DSHADERC_GLSLANG_DIR=${BASEDIR}/glslang -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=install .. -G Ninja
   ninja -C . -j $CORE_COUNT
   ninja -C . install
   # make -j $CORE_COUNT
   # make install
}

function create_moltenvk () {
   rm -rf "${BASEDIR}"/MoltenVK
   echo "Creating local MoltenVK repository (${BASEDIR}/MoltenVK)."
   mkdir -p "${BASEDIR}"/MoltenVK
   cd "${BASEDIR}"/MoltenVK
   git clone --recurse-submodules https://github.com/KhronosGroup/MoltenVK.git "${BASEDIR}"/MoltenVK
}

function update_moltenvk () {
   echo "Updating ${BASEDIR}/MoltenVK"
   cd "${BASEDIR}"/MoltenVK
   git pull
}

function build_moltenvk () {
   echo "Building ${BASEDIR}/MoltenVK"
   cd "${BASEDIR}"/MoltenVK
   ./fetchDependencies --v-lvl-root "${BUILDDIR}" --glslang-root "${BASEDIR}/glslang"
   xcodebuild -project MoltenVKPackaging.xcodeproj \
    GCC_PREPROCESSOR_DEFINITIONS='$GCC_PREPROCESSOR_DEFINITIONS MVK_LOGGING_ENABLED=0' \
    -scheme "MoltenVK (Release)" build
}

INCLUDE_GLSLANG=false
INCLUDE_SHADERC=false
INCLUDE_MOLTENVK=false
NO_SYNC=false
NO_BUILD=false
USE_IMPLICIT_COMPONENT_LIST=true

# Parse options
while [[ $# > 0 ]]
do
  option="$1"

  case $option in
      # options to specify build of glslang components
      -g|--glslang)
      INCLUDE_GLSLANG=true
      USE_IMPLICIT_COMPONENT_LIST=false
      echo "Building glslang ($option)"
      ;;
      # options to specify build of shaderc components
      -c|--shaderc)
      INCLUDE_SHADERC=true
      USE_IMPLICIT_COMPONENT_LIST=false
      echo "Building shaderc ($option)"
      ;;
      # options to specify build of moltenvk components
      -m|--moltenvk)
      if [[ $(uname) == "Darwin" ]]; then
        INCLUDE_MOLTENVK=true
        USE_IMPLICIT_COMPONENT_LIST=false
        echo "Building MoltenVK ($option)"
      fi
      ;;
      # options to specify build of spirv-tools components
      -s|--spirv-tools)
      echo "($option) is deprecated and is no longer necessary"
      ;;
      # option to specify skipping sync from git
      --no-sync)
      NO_SYNC=true
      echo "Skipping sync ($option)"
      ;;
      # option to specify skipping build
      --no-build)
      NO_BUILD=true
      echo "Skipping build ($option)"
      ;;
      *)
      echo "Unrecognized option: $option"
      echo "Usage: update_external_sources.sh [options]"
      echo "  Available options:"
      echo "    -g | --glslang      # enable glslang component"
      echo "    -c | --shaderc      # enable shaderc component"
      if [[ $(uname) == "Darwin" ]]; then
        echo "    -m | --moltenvk     # enable moltenvk component"
      fi
      echo "    --no-sync           # skip sync from git"
      echo "    --no-build          # skip build"
      echo "  If any component enables are provided, only those components are enabled."
      echo "  If no component enables are provided, all components are enabled."
      echo "  Sync uses git to pull a specific revision."
      echo "  Build configures CMake, builds Release."
      exit 1
      ;;
  esac
  shift
done

if [ ${USE_IMPLICIT_COMPONENT_LIST} == "true" ]; then
  echo "Building glslang"
  INCLUDE_GLSLANG=true
  INCLUDE_SHADERC=true
  if [[ $(uname) == "Darwin" ]]; then
    echo "Building MoltenVK"
    INCLUDE_MOLTENVK=true
  fi
fi

if [ ${INCLUDE_GLSLANG} == "true" ]; then
  if [ ${NO_SYNC} == "false" ]; then
    if [ ! -d "${BASEDIR}/glslang" -o ! -d "${BASEDIR}/glslang/.git" -o -d "${BASEDIR}/glslang/.svn" ]; then
       create_glslang
    fi
    update_glslang
  fi
  if [ ${NO_BUILD} == "false" ]; then
    build_glslang
  fi
fi

if [ ${INCLUDE_SHADERC} == "true" ]; then
  if [ ${NO_SYNC} == "false" ]; then
    if [ ! -d "${BASEDIR}/shaderc" -o ! -d "${BASEDIR}/shaderc/.git" -o -d "${BASEDIR}/shaderc/.svn" ]; then
       create_shaderc
    fi
    update_shaderc
  fi
  if [ ${NO_BUILD} == "false" ]; then
    build_shaderc
  fi
fi

if [ ${INCLUDE_MOLTENVK} == "true" ]; then
  if [ ${NO_SYNC} == "false" ]; then
    if [ ! -d "${BASEDIR}/MoltenVK" -o ! -d "${BASEDIR}/MoltenVK/.git" ]; then
      create_moltenvk
    fi
    update_moltenvk
  fi
  if [ ${NO_BUILD} == "false" ]; then
    echo "Building moltenvk"
    build_moltenvk
  fi
fi
