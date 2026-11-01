# DRAFT - always-on EdgeSlicer phone hub (Phase 0 spike, 2026-10-03). Not part of any CI job and never pushed.
#
# Recipe: "hub + one GUI slicer instance under Xvfb + Mesa llvmpipe", from the official Linux AppImage.
# See tests/research_always_on_hub.md, "Spike results (2026-10-03)" for what was measured and what is missing.
#
#   # put the release AppImage next to this file, then (linux/amd64 only):
#   gh release download v2.4.2.0-edge -R aceRage/EdgeSlicer -p "*AppImage" -D docker/hub
#   docker build -t edgeslicer-hub-spike docker/hub --build-arg APPIMAGE=EdgeSlicer_Linux_Ubuntu2404_V2.4.2.0.AppImage
#   docker run -d --name edgeslicer-hub-spike --network host -v es_hub_data:/data edgeslicer-hub-spike
#
# --network host is what a real LAN needs (Bambu SSDP discovery, phone reaching :13640 on the host's LAN
# address). Without it, publish 13640-13659 and the phone link in the log shows the container address.
FROM ubuntu:24.04

ARG APPIMAGE=EdgeSlicer_Linux_Ubuntu2404_V2.4.2.0.AppImage
ENV DEBIAN_FRONTEND=noninteractive

# Run-time libraries the AppImage expects from the host (it bundles its own openssl/curl/webkit-less runtime):
#  - GTK3 + WebKitGTK 4.1: the app links wxGTK, and its web views (device tab) load WebKit
#  - xvfb + mesa: a virtual display and software GL; GTK cannot make a GL context current on an unshown
#    window, so a hidden instance has no OpenGL on Linux (see the study) and the slicer runs "visible" on Xvfb
#  - locales: with no en_US.UTF-8 the first start stops on a "Switching language failed" message box
#  - ca-certificates: the TLS system certificate store question is answered from the seeded config
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl locales tini procps xvfb xauth \
        libgl1-mesa-dri libglx-mesa0 libegl1 libopengl0 libglu1-mesa libgtk-3-0t64 libwebkit2gtk-4.1-0 libsecret-1-0 \
        fonts-dejavu-core \
    && sed -i 's/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

# --appimage-extract needs no FUSE. The tree is run through AppRun (it sets LD_LIBRARY_PATH for the bundled libs).
COPY ${APPIMAGE} /tmp/app.AppImage
RUN chmod +x /tmp/app.AppImage && cd /opt && /tmp/app.AppImage --appimage-extract >/dev/null \
    && mv squashfs-root edgeslicer && chmod -R a+rX edgeslicer && rm /tmp/app.AppImage

COPY seed.conf.json entrypoint.sh /opt/hub/
RUN chmod +x /opt/hub/entrypoint.sh && useradd -m -u 1001 hub && mkdir /data && chown hub /data
USER hub
ENV HOME=/home/hub ES_DATA=/data LANG=en_US.UTF-8

EXPOSE 13640-13659
VOLUME /data
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s CMD curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:13640/ | grep -q 404 || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/hub/entrypoint.sh"]
