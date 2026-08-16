#include "app/app.h"
#include "modes/paint.h"
#include "modes/pattern.h"

/* Register modes here. Order = Tab cycle order and F1..Fn index. */
int main(void) {
    app_mode_t modes[] = {
        pattern_mode(),
        paint_mode(),
    };
    return app_run(modes, (int)(sizeof modes / sizeof modes[0]),
                   &(app_config_t){
                       .width       = 1280,
                       .height      = 720,
                       .title       = "libcg",
                       .transparent = true,
                       .resizable   = true,
                       .high_dpi    = true,
                   });
}
