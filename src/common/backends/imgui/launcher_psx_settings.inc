// PSX Settings composition. The host provider remains the owner of all values.
//
// Three tabs in the Settings view:
//   GENERAL  — System | Audio | Rewind (+ a bus-level Input card when the title
//              has one)
//   DISPLAY  — Display Mode / Aspect / Window / Resolution / VSync beside PGXP
//              and the CRT filter, with Image Quality and Presentation as
//              sub-tabs under it
//   HOTKEYS  — the host-shortcut catalog, on its own tab
//
// Every host option still comes from psx.runtime.settings; this file only
// decides which card a row lands in and how a row is labelled. Per-device
// options (mouse, JogCon, anti-deadzone, fallback Select) are NOT here: they
// belong to the controller being configured and are drawn by
// draw_psx_pad_options() on the Configure page.
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
// The first feature a package exposes, whatever its id happens to be: a package
// such as psx.enhancement.enhanced-audio carries one feature whose id is a host
// detail this file must not have to spell out.
static bool psx_package_feature(LauncherModel* m, const char* package,
                                RecompLauncherCModFeature& out) {
    const auto* p = m->mods;
    if (!p || !p->feature_count || !p->feature_get) return false;
    for (int i=0,n=p->feature_count(p->ctx); i<n; ++i)
        if (p->feature_get(p->ctx,i,&out) && !strcmp(out.package_id,package)) return true;
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

// ---- host options: labels, groups, row drawing ------------------------------
/* The host names a row after what the setting does to the runtime ("Skip BIOS
 * boot animation"). A settings card is written from the player's side, so the
 * rows whose host label describes the mechanism are relabelled here. Keeping the
 * override presentation-side means no host pin change is needed to reword a
 * row, and a console without an override is untouched. */
static const char* psx_option_label(const char* id) {
    if (!strcmp(id,"video.fast_boot")) return "Skip BIOS";
    return nullptr;
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
// Rows that are real but not part of the everyday surface: they sit behind a
// collapsed "Advanced" subsection instead of lengthening the card.
static bool psx_option_advanced(const char* id) {
    return !strcmp(id,"video.bios_hle") || !strcmp(id,"runtime.bios_hle_keep_intro") ||
           !strcmp(id,"video.low_latency_input") || !strcmp(id,"video.fov_scale") ||
           !strcmp(id,"video.texture_lod") || !strcmp(id,"video.anisotropic_filtering") ||
           !strcmp(id,"video.fmv_chroma_smoothing") || !strcmp(id,"video.accurate_blending");
}
static void psx_draw_option(LauncherModel* m, const RecompLauncherCModFeature& f,
                            RecompLauncherCModOption o, const char* label = nullptr) {
    if (label) std::snprintf(o.label, sizeof o.label, "%s", label);
    draw_mod_feature_option(m, f, o);
}
/* One page's rows for one group. `advanced` selects the collapsed-subsection
 * rows (true) or the everyday ones (false). */
static void psx_runtime_rows(LauncherModel* m, const char* page, const char* group,
                             bool advanced) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !psx_feature(m,"psx.runtime.settings",page,f)) return;
    ImGui::PushID(f.package_id); ImGui::PushID(f.id);
    ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (!p->feature_option_get(p->ctx,f.package_id,f.id,i,&o)) continue;
        if (strcmp(psx_option_group(o.id),group) || psx_option_advanced(o.id)!=advanced) continue;
        psx_draw_option(m,f,o,psx_option_label(o.id));
    }
    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID();
}
// A collapsed subsection that only exists when it would have rows in it.
static void psx_advanced_section(LauncherModel* m, const char* page, const char* group,
                                 const char* title) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !psx_feature(m,"psx.runtime.settings",page,f)) return;
    int rows=0;
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (p->feature_option_get(p->ctx,f.package_id,f.id,i,&o) &&
            !strcmp(psx_option_group(o.id),group) && psx_option_advanced(o.id)) ++rows;
    }
    if (!rows) return;
    ImGui::Dummy(ImVec2(0,px(4)));
    if (ImGui::CollapsingHeader(ui_text(title))) psx_runtime_rows(m,page,group,true);
}
// One package's options with no header and no enable row: used where the
// section already carries its own switch.
static void psx_package_options(LauncherModel* m, const char* package) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_package_feature(m,package,f)) return;
    ImGui::PushID(f.package_id); ImGui::PushID(f.id);
    ImGui::BeginDisabled(!launcher_model_can_open_mods(m) || !f.enabled);
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (p->feature_option_get(p->ctx,f.package_id,f.id,i,&o)) psx_draw_option(m,f,o);
    }
    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID();
}
/* A labelled on/off row. The compact PSX layout left-anchors every control,
 * which leaves a lone checkbox floating in the middle of a card; a switch is a
 * square, so it sits against the card's right edge like the other controls. */
static void psx_switch_row(LauncherModel* m,const LauncherTheme& th,const char* label,
                           const RecompLauncherCModFeature& f) {
    row_label_right(label,th,ImGui::GetFrameHeight());
    if (g_psx_compact_settings) {
        const float right=ImGui::GetWindowContentRegionMax().x;
        if (right>ImGui::GetFrameHeight()) ImGui::SetCursorPosX(right-ImGui::GetFrameHeight());
    }
    bool enabled=f.enabled!=0;
    if (ImGui::Checkbox("##switch",&enabled)) change_mod_feature(m,f,enabled);
    if (f.status[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",f.status);
}
/* A package as ONE row — its switch on the right of a label, no subsection and
 * no second layer of options. "Enhanced Audio: [x]" is the whole of that
 * section. */
static void psx_package_switch_row(LauncherModel* m, const char* package,
                                   const char* label) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_enable || !psx_package_feature(m,package,f)) return;
    ImGui::PushID(f.package_id); ImGui::PushID(f.id);
    ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
    psx_switch_row(m,*g_th,label,f);
    ImGui::EndDisabled();
    ImGui::PopID(); ImGui::PopID();
}
// A package promoted to its own card: the name is the eyebrow, its switch a
// labelled row, its options under it. Rewind reads that way instead of as a
// collapsing subsection inside System.
static void psx_feature_card(LauncherModel* m,const LauncherTheme& th,
                             const char* title,const char* package) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_package_feature(m,package,f)) return;
    if (begin_panel(package)) {
        eyebrow(title);
        ImGui::PushID(f.package_id); ImGui::PushID(f.id);
        ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
        psx_switch_row(m,th,"Enabled",f);
        ImGui::BeginDisabled(!f.enabled);
        for (int j=0;j<f.option_count;++j) {
            RecompLauncherCModOption o{};
            if (p->feature_option_get(p->ctx,f.package_id,f.id,j,&o))
                psx_draw_option(m,f,o);
        }
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::PopID(); ImGui::PopID();
        if (f.status[0]) ImGui::TextColored(col(th.text_muted),"%s",f.status);
    }
    end_panel();
}

// ---- GENERAL page ----------------------------------------------------------
static void psx_system_card(LauncherModel* m,const LauncherTheme& th,const char* group) {
    if (begin_panel(group)) {
        draw_system_controls(m,th);
        psx_runtime_rows(m,"general",group,false);
        psx_advanced_section(m,"general",group,"Advanced");
    }
    end_panel();
}
static void psx_audio_card(LauncherModel* m,const LauncherTheme& th,const char* group) {
    if (begin_panel(group)) {
        draw_audio_controls(m,th);
        psx_runtime_rows(m,"general",group,false);
        psx_package_switch_row(m,"psx.enhancement.enhanced-audio","Enhanced Audio");
        psx_advanced_section(m,"general",group,"Advanced");
    }
    end_panel();
}
// Bus-level hardware options stay in Settings (they are not a property of one
// device), in their own card so a 1-player title never shows an empty one.
static bool psx_input_card_available(const LauncherModel* m) {
    return launcher_model_multitap_available(m) ||
           launcher_model_multitap_analog_available(m) ||
           launcher_model_virtual_stylus_available(m);
}
static void psx_input_card(LauncherModel* m,const LauncherTheme& th) {
    if (begin_panel("Input card")) draw_input_controls(m,th);
    end_panel();
}
static void psx_general_page(LauncherModel* m,const LauncherTheme& th) {
    const float width=ImGui::GetContentRegionAvail().x;
    const int cards=psx_input_card_available(m)?4:3;
    const int columns=width>=px(1180)?(cards>3?4:3):width>=px(760)?2:1;
    g_psx_compact_settings=true;
    if (ImGui::BeginTable("psx_general_groups",columns,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); psx_system_card(m,th,"System");
        ImGui::TableNextColumn(); psx_audio_card(m,th,"Audio");
        ImGui::TableNextColumn(); psx_feature_card(m,th,"REWIND","psx.feature.rewind");
        if (psx_input_card_available(m)) { ImGui::TableNextColumn(); psx_input_card(m,th); }
        ImGui::EndTable();
    }
    g_psx_compact_settings=false;
}

// ---- DISPLAY page ----------------------------------------------------------
static void psx_display_mode_row(LauncherModel* m, const LauncherTheme& th) {
    static const SettingsChoice kMode[] = {
        {2, "Exclusive"}, {1, "Borderless"}, {0, "Windowed"}
    };
    const int n=(int)(sizeof kMode/sizeof kMode[0]);
    const float w=settings_fit_w("Display Mode",th,settings_widest_text(n,
        [](int i){ return kMode[i].label; }),true);
    row_label_right("Display Mode",th,w);
    ImGui::SetNextItemWidth(w);
    const char* current="Windowed";
    for (const auto& c:kMode) if (m->s.fullscreen==c.value) current=c.label;
    ImGui::PushID("psx_display_mode");
    if (ImGui::BeginCombo("##mode",ui_text(current))) {
        for (const auto& c:kMode)
            if (ImGui::Selectable(ui_text(c.label),m->s.fullscreen==c.value))
                launcher_model_set_fullscreen(m,c.value);
        ImGui::EndCombo();
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s",ui_text(
            "How the game fills your screen.\n"
            "Exclusive: a dedicated full-screen mode, lowest latency.\n"
            "Borderless: full-screen in a borderless window, fast to alt-tab.\n"
            "Windowed: a resizable window at the size you choose."));
}
static void psx_window_size_row(LauncherModel* m, const LauncherTheme& th) {
    if (!m->has_window_size) return;
    const float ww=settings_cycle_w(m,"Window size",th,
        launcher_model_cycle_window_size,launcher_model_window_size_label);
    row_label_right("Window size",th,ww);
    // Only Windowed uses the size; the two full-screen modes ignore it, so the
    // row greys out instead of offering a number that would change nothing.
    const bool live=m->s.fullscreen==0;
    ImGui::BeginDisabled(!live);
    if (ImGui::Button(ui_text(launcher_model_window_size_label(m)),ImVec2(ww,px(30))) && live)
        launcher_model_cycle_window_size(m);
    ImGui::EndDisabled();
    if (!live && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s",ui_text("Only used in Windowed mode."));
}
/* One package option drawn as an ordinary settings row (label left, value
 * right) with no enable switch above it — the Internal Resolution selector. */
static void psx_package_option_row(LauncherModel* m,const LauncherTheme& th,
                                   const char* package,const char* option_id,
                                   const char* label) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_package_feature(m,package,f)) return;
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (!p->feature_option_get(p->ctx,f.package_id,f.id,i,&o) || strcmp(o.id,option_id)) continue;
        // A feature the player switched off still shows its saved value, greyed.
        const bool inert=o.disabled!=0 || !f.enabled;
        if (inert) ImGui::BeginDisabled();
        if (o.type==RECOMP_MOD_OPTION_CHOICE && p->feature_choice_get && o.choice_count>0) {
            char preview[128]; std::snprintf(preview,sizeof preview,"%s",o.value);
            float widest=0.0f;
            for (int c=0;c<o.choice_count;++c) {
                RecompLauncherCModChoice choice{};
                if (!p->feature_choice_get(p->ctx,f.package_id,f.id,o.id,c,&choice)) continue;
                const char* text=choice.label[0]?choice.label:choice.value;
                const float wtext=ImGui::CalcTextSize(ui_text(text)).x;
                if (wtext>widest) widest=wtext;
                if (!strcmp(choice.value,o.value)) std::snprintf(preview,sizeof preview,"%s",text);
            }
            const float w=settings_fit_w(label,th,widest,true);
            row_label_right(label,th,w);
            ImGui::SetNextItemWidth(w);
            ImGui::PushID(o.id);
            if (ImGui::BeginCombo("##choice",ui_text(preview))) {
                for (int c=0;c<o.choice_count;++c) {
                    RecompLauncherCModChoice choice{};
                    if (!p->feature_choice_get(p->ctx,f.package_id,f.id,o.id,c,&choice)) continue;
                    const bool selected=!strcmp(choice.value,o.value);
                    if (ImGui::Selectable(ui_text(choice.label),selected) && !inert)
                        if (!p->feature_set_option(p->ctx,f.package_id,f.id,o.id,choice.value))
                            mod_note_error(m);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        } else {
            psx_draw_option(m,f,o,label);
        }
        if (inert) ImGui::EndDisabled();
        return;
    }
}
static void psx_vsync_row(LauncherModel* m, const LauncherTheme& th) {
    if (!m->has_vsync || draw_vsync_overridden_row(m,th)) return;
    const int n=(int)(sizeof kVsyncChoices/sizeof kVsyncChoices[0]);
    const float w=settings_fit_w("VSync",th,settings_widest_text(n,
        [](int i){ return kVsyncChoices[i].label; }),true);
    row_label_right("VSync",th,w);
    ImGui::SetNextItemWidth(w);
    ImGui::PushID("psx_vsync");
    if (ImGui::BeginCombo("##vsync",ui_text(launcher_model_vsync_label(m)))) {
        for (const auto& o:kVsyncChoices)
            if (ImGui::Selectable(ui_text(o.label),m->s.vsync==o.v))
                launcher_model_set_vsync(m,o.v);
        ImGui::EndCombo();
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s",ui_text(
            "On: the swap waits for the panel — no tearing.\n"
            "Off: swap immediately — lowest display latency, may tear.\n"
            "Adaptive: vsync while the game keeps up, immediate when it drops.\n"
            "VRR: hand pacing to a variable-refresh display."));
}
/* Display tab, first column: the rows that decide how the picture reaches the
 * screen, in the order a player meets them. Everything else is Advanced. */
static void psx_display_card(LauncherModel* m,const LauncherTheme& th) {
    if (begin_panel("Display")) {
        eyebrow("DISPLAY");
        psx_display_mode_row(m,th);
        draw_aspect_row(m,th);
        psx_window_size_row(m,th);
        psx_package_option_row(m,th,"psx.enhancement.internal-resolution","resolution",
                               "Internal resolution");
        psx_vsync_row(m,th);
        if (ImGui::CollapsingHeader(ui_text("Advanced"))) {
            if (launcher_model_renderer_offered(m)) {
                float widest=0.0f;
                for (int i=0;i<launcher_model_renderer_count(m);++i) {
                    const char* lbl=launcher_model_renderer_label_at(m,i);
                    const float wtext=lbl?ImGui::CalcTextSize(ui_text(lbl)).x:0.0f;
                    if (wtext>widest) widest=wtext;
                }
                const float rw=settings_fit_w("Renderer",th,widest,true);
                row_label_right("Renderer",th,rw);
                ImGui::SetNextItemWidth(rw);
                if (ImGui::BeginCombo("##renderer",ui_text(launcher_model_renderer_label(m)))) {
                    const int n=launcher_model_renderer_count(m);
                    for (int i=0;i<n;++i) {
                        const char* lbl=launcher_model_renderer_label_at(m,i);
                        if (!lbl||!lbl[0]) continue;
                        if (ImGui::Selectable(ui_text(lbl),m->s.renderer==i))
                            launcher_model_set_renderer(m,i);
                    }
                    ImGui::EndCombo();
                }
                const char* note=launcher_model_renderer_note(m);
                if (note&&*note) ImGui::TextColored(col(th.text_muted),"%s",ui_text(note));
            }
            if (m->has_frame_interp && m->s.renderer) {
                row_label_right("Frame interpolation",th,ImGui::GetFrameHeight());
                bool fi=m->s.frame_interp!=0;
                if (ImGui::Checkbox("##fi",&fi)) launcher_model_toggle_frame_interp(m);
                if (m->s.frame_interp) {
                    const float pw=settings_cycle_w(m,"Presentation target",th,
                        launcher_model_cycle_interp_fps,launcher_model_interp_fps_label);
                    row_label_right("Presentation target",th,pw);
                    if (ImGui::Button(ui_text(launcher_model_interp_fps_label(m)),ImVec2(pw,px(30))))
                        launcher_model_cycle_interp_fps(m);
                }
            }
            if (m->has_skip_fmv) {
                row_label_right("Skip FMVs",th,ImGui::GetFrameHeight());
                bool sk=m->s.auto_skip_fmv!=0;
                if (ImGui::Checkbox("##skipfmv",&sk)) launcher_model_toggle_skip_fmv(m);
            }
        }
    }
    end_panel();
}
static void psx_pgxp_card(LauncherModel* m,const LauncherTheme& th) {
    if (begin_panel("PGXP")) {
        eyebrow("PGXP");
        psx_runtime_rows(m,"display","Display",false);
        // The card is already titled PGXP, so the package's own collapsing
        // header would only repeat it: its switch is a labelled row and its
        // options follow directly.
        ImGui::Dummy(ImVec2(0,px(4)));
        psx_package_switch_row(m,"psx.enhancement.pgxp","CPU/culling add-on");
        psx_package_options(m,"psx.enhancement.pgxp");
        psx_advanced_section(m,"display","Display","Advanced");
    }
    end_panel();
}
static void psx_crt_card(LauncherModel* m,const LauncherTheme& th) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_package_feature(m,"psx.enhancement.crt-filter",f)) return;
    if (begin_panel("CRT Filter")) {
        eyebrow("CRT FILTER");
        ImGui::PushID(f.package_id); ImGui::PushID(f.id);
        ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
        psx_switch_row(m,th,"Enabled",f);
        ImGui::BeginDisabled(!f.enabled);
        for (int j=0;j<f.option_count;++j) {
            RecompLauncherCModOption o{};
            if (p->feature_option_get(p->ctx,f.package_id,f.id,j,&o)) psx_draw_option(m,f,o);
        }
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::PopID(); ImGui::PopID();
    }
    end_panel();
}
static void psx_image_quality_tab(LauncherModel* m,const LauncherTheme& th) {
    // The filters are not optional: this page IS the image pipeline, so a row
    // that can switch the whole thing off would only be a way to lose the
    // settings configured here. Turn the package on if an older profile left it
    // off, then edit its options directly.
    const auto* mods=m->mods; RecompLauncherCModFeature f{};
    if (mods && psx_package_feature(m,"psx.enhancement.image-filtering",f) && !f.enabled)
        change_mod_feature(m,f,true);
    const float full=ImGui::GetContentRegionAvail().x;
    const float gap=px(th.spacing_md);
    // Half the page: this card is a short list, and a half-width column keeps it
    // beside the space the other sub-tabs use.
    const float col=full>px(640)?(full-gap)*0.5f:full;
    begin_container("psx_iq",ImVec2(col,0),ImGuiChildFlags_AutoResizeY);
    if (begin_panel("Image Quality")) {
        eyebrow("IMAGE QUALITY");
        psx_package_options(m,"psx.enhancement.image-filtering");
        psx_runtime_rows(m,"display","Image",false);
        psx_advanced_section(m,"display","Image","Advanced");
    }
    end_panel();
    end_container();
}
static void psx_presentation_tab(LauncherModel* m,const LauncherTheme& th) {
    if (begin_panel("Presentation")) {
        eyebrow("PRESENTATION");
        psx_runtime_rows(m,"display","Effects",false);
        psx_advanced_section(m,"display","Effects","Advanced");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0,px(th.spacing_md)));
    psx_crt_card(m,th);
}
/* Two columns fill the page in the full-screen launcher; in a window the cards
 * follow one another, because a window is narrow in both directions. */
static bool psx_wide_launcher(const LauncherModel* m,float floor) {
    return m->s.fullscreen!=0 && ImGui::GetContentRegionAvail().x>=px(floor);
}
static void psx_display_tab(LauncherModel* m,const LauncherTheme& th) {
    const float full=ImGui::GetContentRegionAvail().x;
    const float gap=px(th.spacing_md);
    // DISPLAY on the left, its PGXP companion on the right, as soon as the page
    // is wide enough to hold both; a genuinely narrow page stacks them.
    if (full>=px(760)) {
        const float col=(full-gap)*0.5f;
        begin_container("psxdisp_l",ImVec2(col,0),ImGuiChildFlags_AutoResizeY);
        psx_display_card(m,th);
        end_container();
        ImGui::SameLine(0,gap);
        begin_container("psxdisp_r",ImVec2(col,0),ImGuiChildFlags_AutoResizeY);
        psx_pgxp_card(m,th);
        end_container();
    } else {
        psx_display_card(m,th);
        ImGui::Dummy(ImVec2(0,gap));
        psx_pgxp_card(m,th);
    }
}
static void psx_display_page(LauncherModel* m,const LauncherTheme& th) {
    draw_psx_presets(m);
    static int last_display_tab_frame=-2;
    const int frame=ImGui::GetFrameCount();
    const bool first=frame!=last_display_tab_frame+1;
    last_display_tab_frame=frame;
    g_psx_compact_settings=true;
    if (ImGui::BeginTabBar("PSX display tabs")) {
        if (ImGui::BeginTabItem(ui_text("Display"),nullptr,
                                first?ImGuiTabItemFlags_SetSelected:0)) {
            psx_display_tab(m,th);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_text("Image Quality"))) {
            psx_image_quality_tab(m,th);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_text("Presentation"))) {
            psx_presentation_tab(m,th);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    g_psx_compact_settings=false;
}

// ---- Configure page: per-device host options -------------------------------
/* Mouse, JogCon, anti-deadzone and the fallback Select chord are properties of
 * the DEVICE in the player's hands, not of the machine, so they are edited next
 * to that device on the Configure page. Gating follows what the player has: the
 * mouse rows appear for a PS1 Mouse seat, the JogCon row only for a pad that
 * reports itself as one. */
static bool psx_name_is_jogcon(const char* name) {
    if (!name || !name[0]) return false;
    static const char kNeedle[]="jogcon";
    for (const char* s=name; *s; ++s) {
        bool match=true;
        for (int i=0;i<6 && match;++i) {
            char c=s[i];
            if (!c) { match=false; break; }
            if (c>='A'&&c<='Z') c=(char)(c-'A'+'a');
            match = c==kNeedle[i];
        }
        if (match) return true;
    }
    return false;
}
bool psx_player_has_jogcon(const LauncherModel* m,int p) {
    return m && p>=0 && p<RECOMP_LAUNCHER_MAX_PLAYERS &&
           m->s.player_src[p]==2 && psx_name_is_jogcon(m->player_pad_name[p]);
}
void draw_psx_pad_options(LauncherModel* m,const LauncherTheme& th,int p) {
    const auto* mods=m->mods; RecompLauncherCModFeature f{};
    if (!mods || !mods->feature_option_get ||
        !psx_feature(m,"psx.runtime.settings","general",f)) return;
    const int src=m->s.player_src[p];
    const bool mouse=src==RECOMP_LAUNCHER_SRC_MOUSE;
    const bool jogcon=psx_player_has_jogcon(m,p);
    ImGui::PushID(f.package_id); ImGui::PushID(f.id);
    ImGui::BeginDisabled(!launcher_model_can_open_mods(m));
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (!mods->feature_option_get(mods->ctx,f.package_id,f.id,i,&o)) continue;
        const bool is_mouse=!strncmp(o.id,"controller.mouse_",17);
        const bool is_jogcon=!strcmp(o.id,"controller.experimental_jogcon_force_feedback_invert");
        if (is_mouse && !mouse) continue;
        if (is_jogcon && !jogcon) continue;
        if (!is_mouse && !is_jogcon && strncmp(o.id,"controller.",11)) continue;
        psx_draw_option(m,f,o);
    }
    ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID();
}

// ---- entry point -----------------------------------------------------------
static void draw_psx_settings_page(LauncherModel* m,const LauncherTheme& th,int page) {
    if (page==PSX_SETTINGS_GENERAL) psx_general_page(m,th);
    else if (page==PSX_SETTINGS_DISPLAY) psx_display_page(m,th);
    else panel_hotkeys_draw(m,&th);
}
