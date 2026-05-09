#pragma once

namespace whiteout::flakes::renderer { class RenderService; }

namespace whiteout::flakes {

void LoadSettingsIni(renderer::RenderService& service);

void SaveSettingsIni(const renderer::RenderService& service);

}
