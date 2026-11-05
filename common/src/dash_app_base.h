// xenondash common - classe de base des applications Dashboard (une par version).
//
// Regroupe ce qui ne dépend pas de la version : montage de la flash (\SystemRoot), interfaces XAM
// ImGui de remplacement, profil, disposition clavier, audio silencieux optionnel.
// Chaque version dérive de DashAppBase en donnant le nom du XEX et sa configuration d'image PPC.

#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <rex/rex_app.h>

namespace xenondash {

class DashAppBase : public rex::ReXApp {
 public:
  // app_name : nom du projet ReXGlue ; xex_name : exécutable du Dashboard sous game:\ (ex. "dash.xex").
  DashAppBase(rex::ui::WindowedAppContext& ctx, std::string_view app_name,
              rex::PPCImageInfo ppc_info, std::string xex_name)
      : rex::ReXApp(ctx, app_name, ppc_info), xex_name_(std::move(xex_name)) {}


 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPostSetup() override;

 private:
  std::string xex_name_;
};

}  // namespace xenondash
