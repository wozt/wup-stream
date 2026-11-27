#include "wup.h"
#include "gfx.h"

int main(void)
{
    int rc = wup_camera_open();
    if (rc < 0)
        return 1;

    for (;;) {
        wup_camera_draw(107, 0, 0);
        gfx_present_all();
        wup_sleep(16);
    }

    wup_camera_close();
    return 0;
}
