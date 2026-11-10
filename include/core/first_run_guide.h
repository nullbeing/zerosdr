#ifndef FIRST_RUN_GUIDE_H
#define FIRST_RUN_GUIDE_H

class Framebuffer;
class InputHandler;

/** True when the onboarding guide should be shown on this start. */
bool guideShouldShow();

/** Persist the "show the guide on next start" choice. */
void setGuideShow(bool show);

/** Full-screen first-run guide. Returns when the user finishes. */
void runFirstRunGuide(Framebuffer& fb, InputHandler& input);

#endif // FIRST_RUN_GUIDE_H
