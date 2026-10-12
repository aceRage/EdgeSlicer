Moonraker answers for MoonrakerProgress (tests/slic3rutils/moonraker_progress_tests.cpp).

Hand-built from the shapes Moonraker documents and the U1's Device page reads
(/printer/objects/query with print_stats, display_status and virtual_sdcard;
/server/files/metadata). Not captured from a printer: no serial numbers, no addresses.

u1_metadata.json              the job's file: thumbnails before the body (gcode_start_byte 330500),
                              a config block after it, estimated_time 4948 s
u1_query_early.json           the owner's report, 2026-10-11: layer 11/322, body 3 %, file share 8 %,
                              print_duration such that the old linear ETA said 4 h 16 m; the
                              Device page said 3 % and 1 h 20 m
u1_query_mid.json             the same job half way through the body
u1_query_complete.json        finished: file_position at the end of the file
u1_query_standby.json         idle printer, nothing loaded
metadata_no_body.json         a file Moonraker could not read the slicer's numbers from
metadata_404.json             the error a missing file answers
klipper_query_no_sdcard.json  a generic Klipper host that answers display_status only, 2 % in
