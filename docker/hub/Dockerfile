# Always-on EdgeSlicer phone hub: the hub plus one slicer instance under Xvfb + Mesa llvmpipe, from the
# Linux AppImage, in SERVICE MODE (--hub-service): no wizard, no prompts, a minimal config seeded on first start,
# the instance supervised and respawned by the hub. See README.md in this folder, and
# tests/research_always_on_hub.md ("9. Spike results", "10. Linux headless blockers").
#
#   # put an AppImage built from this branch (it carries go2rtc) next to this file, then (linux/amd64):
#   # (the AppImage carries go2rtc and an LGPL ffmpeg with libopenh264; the arm64 image is built the same way from the arm64 AppImage)
#   docker build -t edgeslicer-hub docker/hub --build-arg APPIMAGE=EdgeSlicer_Linux_AppImage_Ubuntu2404_<ver>.AppImage
#   docker run -d --name edgeslicer-hub --network host -v edgeslicer-hub-data:/data edgeslicer-hub
#
# --network host is what a real LAN needs (Bambu SSDP discovery, the phone reaching :13640 on the host's own
# address). Without it, publish the hub port and tell the hub which address phones should use:
#   docker run -d -p 13640:13640 -e EDGESLICER_HUB_PUBLIC_HOST=192.168.1.50 ...
#   (host:port when the published port differs: -p 13650:13640 -e EDGESLICER_HUB_PUBLIC_HOST=192.168.1.50:13650)
FROM ubuntu:24.04

ARG APPIMAGE=EdgeSlicer_Linux_AppImage_Ubuntu2404.AppImage
# 0 = do not generate en_US.UTF-8 (a minimal image): service mode then logs the missing locale and carries on in
# English instead of stopping on the "Switching language failed" box. Used to test exactly that.
ARG GEN_LOCALE=1
ENV DEBIAN_FRONTEND=noninteractive

# Run-time libraries the AppImage expects from the host (it bundles its own openssl/curl/webkit-less runtime):
#  - GTK3 + WebKitGTK 4.1: the app links wxGTK, and its web views (device tab) load WebKit
#  - xvfb + mesa: a virtual display and software GL. GTK cannot make a GL context current on an unshown
#    window, so a service-mode instance shows its window on the (virtual) display
#  - libopengl0 libglu1-mesa: "missing host OpenGL runtime library libOpenGL.so.0" without them
#  - locales: not needed any more (service mode falls back to English silently); en_US.UTF-8 is generated
#    anyway so the interface and number formats come up normally
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl locales tini procps xvfb xauth \
        libgl1-mesa-dri libglx-mesa0 libegl1 libopengl0 libglu1-mesa libgtk-3-0t64 libwebkit2gtk-4.1-0 libsecret-1-0 \
        fonts-dejavu-core \
    && if [ "$GEN_LOCALE" = "1" ]; then sed -i 's/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen && locale-gen en_US.UTF-8; fi \
    && rm -rf /var/lib/apt/lists/*

# --appimage-extract needs no FUSE. The tree is run through AppRun (it sets LD_LIBRARY_PATH for the bundled libs).
COPY ${APPIMAGE} /tmp/app.AppImage
RUN chmod +x /tmp/app.AppImage && cd /opt && /tmp/app.AppImage --appimage-extract >/dev/null \
    && mv squashfs-root edgeslicer && chmod -R a+rX edgeslicer && rm /tmp/app.AppImage \
    && (test -x /opt/edgeslicer/resources/tools/go2rtc/go2rtc && test -x /opt/edgeslicer/resources/tools/go2rtc/ffmpeg \
        || (echo "this AppImage carries no resources/tools/go2rtc/go2rtc and ffmpeg - use one built from the feat/linux-headless-hub branch or later" >&2; exit 1))

COPY entrypoint.sh /opt/hub/
RUN chmod +x /opt/hub/entrypoint.sh && useradd -m -u 1001 hub && mkdir /data && chown hub /data
USER hub
# EDGESLICER_SERVICE=1 is what makes this a service: the hub passes it to the slicer instance it starts.
ENV HOME=/home/hub ES_DATA=/data LANG=en_US.UTF-8 EDGESLICER_SERVICE=1

EXPOSE 13640-13659
VOLUME /data
HEALTHCHECK --interval=30s --timeout=5s --start-period=90s CMD curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:13640/ | grep -q 404 || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/hub/entrypoint.sh"]
