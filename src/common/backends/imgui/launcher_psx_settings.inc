// PSX Settings composition. The host provider remains the owner of all values.
struct PsxPresetValue { const char *package, *feature, *option, *faithful, *best; };
static const PsxPresetValue kPsxPreset[] = {
    {"psx.enhancement.internal-resolution", "internal-resolution", "resolution", "1x", "4x"},
    {"psx.enhancement.image-filtering", "image-filtering", "smoothing", "sharp", "smooth"},
    {"psx.enhancement.image-filtering", "image-filtering", "texture_filtering", "nearest", "bilinear"},
    {"psx.enhancement.image-filtering", "image-filtering", "fmv_filtering", "nearest", "bicubic"},
    {"psx.runtime.settings", "display", "video.geometry_correction", "false", "true"},
    {"psx.runtime.settings", "display", "video.perspective_texturing", "false", "true"},
    {"psx.runtime.settings", "display", "video.antialiasing_mode", "off", "fxaa_hq"},
    {"psx.runtime.settings", "display", "video.dithering", "on", "fine"},
    {"psx.runtime.settings", "display", "video.texture_lod", "off", "mipmap"},
    {"psx.runtime.settings", "display", "video.anisotropic_filtering", "1", "16"},
    {"psx.runtime.settings", "display", "video.fmv_chroma_smoothing", "false", "true"},
    {"psx.runtime.settings", "display", "video.accurate_blending", "true", "true"},
    {"psx.runtime.settings", "display", "video.bloom", "0", "0"},
    {"psx.runtime.settings", "display", "video.post_shader", "", ""},
    {"psx.runtime.settings", "display", "video.supersample", "1", "1"},
};
// PGXP's explicit vertex/texture overrides are visual only. Keep the separate
// mod off in both presets so its saved precise-culling hack cannot change NCLIP.
// Its CPU/culling options and the title's PGXP tolerance/depth tuning are retained.
struct PsxPresetSwitch { const char *package, *feature; int enabled; };
static const PsxPresetSwitch kPsxPresetSwitches[] = {
    {"psx.enhancement.internal-resolution", "internal-resolution", 1},
    {"psx.enhancement.image-filtering", "image-filtering", 1},
    {"psx.enhancement.pgxp", "pgxp", 0},
    {"psx.enhancement.crt-filter", "crt-filter", 0},
};
static bool psx_feature(LauncherModel* m, const char* package, const char* id,
                        RecompLauncherCModFeature& out) {
    const auto* p = m->mods;
    if (!p || !p->feature_count || !p->feature_get) return false;
    for (int i=0,n=p->feature_count(p->ctx); i<n; ++i)
        if (p->feature_get(p->ctx,i,&out) && !strcmp(out.package_id,package) && !strcmp(out.id,id)) return true;
    return false;
}
static bool psx_preset_available(LauncherModel* m) {
    const auto* p=m->mods;
    if (!p || !p->feature_option_get || !p->feature_set_option || !p->feature_enable ||
        !launcher_model_can_open_mods(m) || !m->has_renderer || !m->has_frame_interp) return false;
    for (const auto& item:kPsxPreset) {
        RecompLauncherCModFeature f{};
        if (!psx_feature(m,item.package,item.feature,f)) return false;
        bool found=false;
        for (int i=0;i<f.option_count;++i) {
            RecompLauncherCModOption o{};
            if (p->feature_option_get(p->ctx,item.package,item.feature,i,&o) && !strcmp(o.id,item.option)) {
                if (o.disabled) return false;
                if (o.type==RECOMP_MOD_OPTION_CHOICE) {
                    bool faithful=false,best=false;
                    if (!p->feature_choice_get) return false;
                    for (int c=0;c<o.choice_count;++c) {
                        RecompLauncherCModChoice choice{};
                        if (!p->feature_choice_get(p->ctx,item.package,item.feature,item.option,c,&choice)) return false;
                        faithful |= !strcmp(choice.value,item.faithful);
                        best |= !strcmp(choice.value,item.best);
                    }
                    if (!faithful || !best) return false;
                }
                found=true; break;
            }
        }
        if (!found) return false;
    }
    for (const auto& item:kPsxPresetSwitches) {
        RecompLauncherCModFeature f{};
        if (!psx_feature(m,item.package,item.feature,f)) return false;
    }
    return true;
}
static int psx_current_preset(LauncherModel* m) {
    if (!psx_preset_available(m)) return -1;
    const auto* p=m->mods;
    bool faithful=true,best=true;
    for (const auto& item:kPsxPreset) {
        RecompLauncherCModFeature f{}; psx_feature(m,item.package,item.feature,f);
        for (int i=0;i<f.option_count;++i) {
            RecompLauncherCModOption o{};
            if (p->feature_option_get(p->ctx,item.package,item.feature,i,&o) && !strcmp(o.id,item.option)) {
                faithful &= !strcmp(o.value,item.faithful);
                best &= !strcmp(o.value,item.best);
            }
        }
    }
    for (const auto& item:kPsxPresetSwitches) {
        RecompLauncherCModFeature f{}; psx_feature(m,item.package,item.feature,f);
        faithful &= f.enabled==item.enabled; best &= f.enabled==item.enabled;
    }
    // Both are 4:3, with original motion and no adaptive widescreen or shader.
    if (m->s.renderer!=1 || m->s.aspect_index || m->s.widescreen ||
        m->s.frame_interp || m->s.frame_generation) return -1;
    return faithful ? 0 : best ? 1 : -1;
}
static bool psx_apply_preset(LauncherModel* m, bool best) {
    if (!psx_preset_available(m)) return false;
    const auto* p=m->mods;
    for (const auto& item:kPsxPreset)
        if (!p->feature_set_option(p->ctx,item.package,item.feature,item.option,best?item.best:item.faithful)) {
            mod_note_error(m); return false; // Never label a partial/rejected write as a preset.
        }
    for (const auto& item:kPsxPresetSwitches)
        if (!p->feature_enable(p->ctx,item.package,item.feature,item.enabled)) { mod_note_error(m); return false; }
    launcher_model_set_renderer(m,1); // PSX ABI: software=0, OpenGL=1.
    m->s.aspect_index=0; m->s.widescreen=0;
    m->s.frame_interp=0; m->s.frame_generation=0;
    if (m->has_bios && m->bios_verify_for_rom_cb) launcher_model_refresh_bios_status(m);
    return true;
}
static void draw_psx_presets(LauncherModel* m) {
    const int selected=psx_current_preset(m);
    ImGui::TextUnformatted(ui_text("Picture preset")); ImGui::SameLine();
    ImGui::TextDisabled("%s",ui_text(selected<0?"Custom":selected?"Best":"Faithful"));
    ImGui::BeginDisabled(!psx_preset_available(m));
    if (ImGui::Button(ui_text("Faithful"))) psx_apply_preset(m,false);
    ImGui::SameLine();
    if (ImGui::Button(ui_text("Best"))) psx_apply_preset(m,true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(px(360));
        ImGui::TextUnformatted("Faithful: native resolution, 4:3, original geometry, nearest filtering and PS1 dithering.\n"
            "Best: 4x resolution, corrected geometry/textures and higher-quality filtering. More GPU work; hardware may limit resolution.\n"
            "Both use 4:3 and keep original game timing and motion. Audio, bindings, cards and game-specific tuning are retained. "
            "Applies next launch; explicit command-line and environment overrides still win.");
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    ImGui::Spacing();
}
// Split the runtime provider's two large lists into compact, related groups.
static const char* psx_option_group(const char* id) {
    if (!strncmp(id,"audio.",6)) return "Audio";
    if (!strncmp(id,"controller.",11)) return "Input";
    if (!strcmp(id,"video.geometry_correction") || !strcmp(id,"video.perspective_texturing")) return "Picture";
    if (!strncmp(id,"video.pgxp_",11)) return "Display";
    if (!strcmp(id,"video.fast_boot") || !strcmp(id,"video.bios_hle") ||
        !strncmp(id,"runtime.",8) || !strncmp(id,"memcard.",8) || !strcmp(id,"video.low_latency_input")) return "System";
    if (!strcmp(id,"video.fov_scale") || !strcmp(id,"video.texture_lod") ||
        !strcmp(id,"video.anisotropic_filtering") || !strcmp(id,"video.fmv_chroma_smoothing") ||
        !strcmp(id,"video.accurate_blending") || !strcmp(id,"video.dithering")) return "Image";
    return "Effects";
}
static void psx_runtime_group(LauncherModel* m,const char* page,const char* group) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !psx_feature(m,"psx.runtime.settings",page,f)) return;
    ImGui::PushID(f.package_id); ImGui::PushID(f.id);
    ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (p->feature_option_get(p->ctx,f.package_id,f.id,i,&o) && !strcmp(psx_option_group(o.id),group))
            draw_mod_feature_option(m,f,o);
    }
    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID();
}
static void psx_package_group(LauncherModel* m,const char* package) {
    const auto* p=m->mods;
    if (!p || !p->feature_count || !p->feature_get || !p->feature_option_get) return;
    for (int i=0,n=p->feature_count(p->ctx);i<n;++i) {
        RecompLauncherCModFeature f{};
        if (!p->feature_get(p->ctx,i,&f) || strcmp(package,f.package_id)) continue;
        ImGui::PushID(f.package_id); ImGui::PushID(f.id);
        if (ImGui::CollapsingHeader(ui_text(f.name),ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
            bool enabled=f.enabled!=0;
            const bool pgxp=!strcmp(package,"psx.enhancement.pgxp");
            row_label_right(pgxp?"CPU/culling add-on":"Enabled",*g_th,ImGui::GetFrameHeight());
            if (ImGui::Checkbox("##enabled",&enabled)) change_mod_feature(m,f,enabled);
            if (pgxp && ImGui::IsItemHovered()) ImGui::SetTooltip("The visual vertex and texture overrides above work independently. This add-on also enables its saved CPU propagation and precise-culling options. Precise culling can change game math.");
            ImGui::BeginDisabled(!f.enabled);
            for (int j=0;j<f.option_count;++j) {
                RecompLauncherCModOption o{};
                if (p->feature_option_get(p->ctx,f.package_id,f.id,j,&o)) draw_mod_feature_option(m,f,o);
            }
            ImGui::EndDisabled(); ImGui::EndDisabled();
            if (f.status[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",f.status);
        }
        ImGui::PopID(); ImGui::PopID();
    }
}
static void draw_psx_picture_controls(LauncherModel* m) {
    psx_package_group(m,"psx.enhancement.internal-resolution");
    psx_runtime_group(m,"display","Picture");
    psx_package_group(m,"psx.enhancement.pgxp");
}
static void psx_settings_card(LauncherModel* m,const LauncherTheme& th,const char* group) {
    if (begin_panel(group)) {
        if (!strcmp(group,"Display")) {
            draw_display_controls(m,th);
            if (ImGui::CollapsingHeader("PGXP tuning",ImGuiTreeNodeFlags_DefaultOpen)) psx_runtime_group(m,"display",group);
        } else if (!strcmp(group,"Image")) {
            eyebrow("IMAGE QUALITY");
            psx_package_group(m,"psx.enhancement.image-filtering");
            psx_runtime_group(m,"display",group);
        } else if (!strcmp(group,"Effects")) {
            eyebrow("EFFECTS AND PRESENTATION");
            psx_runtime_group(m,"display",group);
            psx_package_group(m,"psx.enhancement.crt-filter");
        } else {
            if (!strcmp(group,"System")) draw_system_controls(m,th);
            if (!strcmp(group,"Audio")) draw_audio_controls(m,th);
            if (!strcmp(group,"Input")) { draw_input_controls(m,th); }
            psx_runtime_group(m,"general",group);
            if (!strcmp(group,"Audio")) psx_package_group(m,"psx.enhancement.enhanced-audio");
            if (!strcmp(group,"System")) psx_package_group(m,"psx.feature.rewind");
        }
    }
    end_panel();
}
static void draw_psx_settings_page(LauncherModel* m,const LauncherTheme& th,bool display) {
    if (display) draw_psx_presets(m);
    const float width=ImGui::GetContentRegionAvail().x;
    const int columns=width>=px(1500)?3:width>=px(840)?2:1;
    g_psx_compact_settings=true;
    if (ImGui::BeginTable("psx_settings_groups",columns,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); psx_settings_card(m,th,display?"Display":"System");
        if (columns>1) ImGui::TableNextColumn();
        psx_settings_card(m,th,display?"Image":"Audio");
        if (columns==3 || columns==1) ImGui::TableNextColumn();
        psx_settings_card(m,th,display?"Effects":"Input");
        ImGui::EndTable();
    }
    g_psx_compact_settings=false;
    if (!display) panel_hotkeys_draw(m,&th);
}
