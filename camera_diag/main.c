#include "wup.h"
#include "gfx.h"

WUP_APP("Camera Diagnostic");

int main(void)
{
    if (wup_camera_open() < 0)
        return 1;

    while ((wup_buttons() & (WUP_HOME | WUP_B))
           != (WUP_HOME | WUP_B)) {
        if (wup_camera_draw(107, 0, 0) > 0)
            gfx_present_all();

        wup_sleep(5);
    }

    wup_camera_close();
    return 0;
}
