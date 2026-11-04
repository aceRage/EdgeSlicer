"""The emulator's core: the command log, the HTTP listeners, the safety rails and the supervisor.

Standard library only (design D1). Nothing in here imports EdgeSlicer code: the emulator must not
share the product's parsers, or it would agree with the product's bugs (design section 8).
"""
