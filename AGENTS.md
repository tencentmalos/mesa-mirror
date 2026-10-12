# AGENTS.md

This is tencentmalos/mesa-mirror, a private Turnip fork used by shadPS4 and
Azahar. Nothing here is submitted to Mesa directly.

- Do not open merge requests, comment or otherwise take part on Mesa's GitLab.
  Anything sent upstream is reworded and submitted by the user.
- Label AI work with the commit trailer `Generated-by: LLM` (the agent wrote
  the code) or `Assisted-by: LLM` (the agent helped the user write it).
- Fork changes serve different consumers: shadPS4 game workarounds, Azahar and
  Swan XR, KGSL diagnostics. Make each one identifiable:
  - comment fork code with its consumer, the hardware, firmware or KGSL
    behaviour it handles, and the evidence when that is not obvious;
  - say the consumer and the problem in the commit message;
  - give workarounds a `TU_DEBUG` switch and log once when one is active, so
    A/B runs and logs show which fork behaviour applied.
- Do not change `src/asahi` or `src/gallium/drivers/asahi`; those projects do
  not accept AI-written code (https://asahilinux.org/llm-policy/).
