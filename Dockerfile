from ubuntu:22.04 as vkbase
RUN apt-get update && \
    apt-get install -y --no-install-recommends wget ca-certificates && rm -rf /var/lib/apt/lists/

RUN wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc | tee /etc/apt/trusted.gpg.d/lunarg.asc
RUN wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.3.261-jammy.list https://packages.lunarg.com/vulkan/1.3.261/lunarg-vulkan-1.3.261-jammy.list

from vkbase as builder
RUN \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libwayland-dev \
    libxcb1-dev \
    libx11-dev \
    pkg-config \
    libvulkan-dev \
    python3 \
    python3-distutils \
    python-is-python3 \
    ninja-build && rm -rf /var/lib/apt/lists/*

COPY vk_video_decoder/external_revisions/ vk_video_decoder/external_revisions/
COPY vk_video_decoder/update_external_sources.sh vk_video_decoder/update_external_sources.sh
RUN cd vk_video_decoder/ && bash ./update_external_sources.sh
COPY vk_video_decoder/cmake vk_video_decoder/cmake
COPY vk_video_decoder/demos vk_video_decoder/demos
COPY vk_video_decoder/include vk_video_decoder/include
COPY vk_video_decoder/layers vk_video_decoder/layers
COPY vk_video_decoder/libs vk_video_decoder/libs
COPY vk_video_decoder/libs vk_video_decoder/libs
COPY vk_video_decoder/CMakeLists.txt vk_video_decoder/CMakeLists.txt
COPY vk_video_decoder/scripts vk_video_decoder/scripts
COPY common common

RUN cmake -B build \
	-S vk_video_decoder \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -G Ninja \
        .
RUN cmake --build build --parallel && cmake --install build

from vkbase as runner
RUN --mount=type=cache,target=/var/cache/apt apt-get update && \
    apt-get install -y --no-install-recommends \
    libavcodec58 \
    libavformat58 \
    libvulkan1 \
    libxcb1 \
    libx11-6 \
    libglvnd0 \
    libgl1 \
    libglx0 \
    libegl1  \
    libgles2  \
    libxcb1-dev && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /build/demos/
COPY --from=builder /build /build
RUN mkdir -p /vk_video_decoder/external/shaderc/build/install/lib/
COPY --from=builder /vk_video_decoder/external/shaderc/build/install/lib/libshaderc_shared.so.1 /vk_video_decoder/external/shaderc/build/install/lib/
    
ENV NVIDIA_VISIBLE_DEVICES all
ENV NVIDIA_DRIVER_CAPABILITIES compute,utility,graphics,video,display
ENTRYPOINT ["/build/demos/vk-video-dec-test"]