// PSX Settings composition. The host provider remains the owner of all values.
//
// The host's player settings arrive as the package psx.runtime.settings: one
// feature per Settings page (general, display and, from hosts that have it,
// pgxp; the feature's group is the page name) and one option per setting. An
// option's group is its heading on that page. A heading equal to the page name,
// or empty from an older host, marks the page's main rows, which keep their
// place in the page's existing cards; every other heading gets a card of its
// own, in the order the host lists it.
static const char kPsxSettingsPackage[]="psx.runtime.settings";

static bool psx_feature(LauncherModel* m, const char* package, const char* id,
                        RecompLauncherCModFeature& out) {
    const auto* p = m->mods;
    if (!p || !p->feature_count || !p->feature_get) return false;
    for (int i=0,n=p->feature_count(p->ctx); i<n; ++i)
        if (p->feature_get(p->ctx,i,&out) && !strcmp(out.package_id,package) && !strcmp(out.id,id)) return true;
    return false;
}
static bool psx_has_pgxp_page(LauncherModel* m) {
    RecompLauncherCModFeature f{};
    return psx_feature(m,kPsxSettingsPackage,"pgxp",f);
}
/* Finds one option. A null feature searches every feature of the package, so a
 * setting the host moves to another page keeps its preset entry. */
static bool psx_find_option(LauncherModel* m, const char* package, const char* feature,
                            const char* option, RecompLauncherCModFeature& f,
                            RecompLauncherCModOption& o) {
    const auto* p = m->mods;
    if (!p || !p->feature_count || !p->feature_get || !p->feature_option_get) return false;
    for (int i=0,n=p->feature_count(p->ctx); i<n; ++i) {
        if (!p->feature_get(p->ctx,i,&f) || strcmp(f.package_id,package) || (feature && strcmp(f.id,feature)))
            continue;
        for (int j=0; j<f.option_count; ++j)
            if (p->feature_option_get(p->ctx,f.package_id,f.id,j,&o) && !strcmp(o.id,option)) return true;
    }
    return false;
}
static bool psx_choice_has(LauncherModel* m, const RecompLauncherCModFeature& f,
                           const RecompLauncherCModOption& o, const char* value) {
    if (o.type!=RECOMP_MOD_OPTION_CHOICE) return true;
    const auto* p = m->mods;
    if (!p->feature_choice_get) return false;
    for (int c=0; c<o.choice_count; ++c) {
        RecompLauncherCModChoice choice{};
        if (p->feature_choice_get(p->ctx,f.package_id,f.id,o.id,c,&choice) && !strcmp(choice.value,value)) return true;
    }
    return false;
}

// ---- Picture presets -------------------------------------------------------
struct PsxPresetValue { const char *package, *feature, *option, *faithful, *best; };
static const PsxPresetValue kPsxPreset[] = {
    {"psx.enhancement.image-filtering", "image-filtering", "smoothing", "sharp", "smooth"},
    {"psx.enhancement.image-filtering", "image-filtering", "texture_filtering", "nearest", "bilinear"},
    {"psx.enhancement.image-filtering", "image-filtering", "fmv_filtering", "nearest", "bicubic"},
    {kPsxSettingsPackage, nullptr, "video.geometry_correction", "false", "true"},
    {kPsxSettingsPackage, nullptr, "video.perspective_texturing", "false", "true"},
    {kPsxSettingsPackage, nullptr, "video.antialiasing_mode", "off", "fxaa_hq"},
    {kPsxSettingsPackage, nullptr, "video.dithering", "on", "fine"},
    {kPsxSettingsPackage, nullptr, "video.texture_lod", "off", "mipmap"},
    {kPsxSettingsPackage, nullptr, "video.anisotropic_filtering", "1", "16"},
    {kPsxSettingsPackage, nullptr, "video.fmv_chroma_smoothing", "false", "true"},
    {kPsxSettingsPackage, nullptr, "video.accurate_blending", "true", "true"},
    {kPsxSettingsPackage, nullptr, "video.bloom", "0", "0"},
    {kPsxSettingsPackage, nullptr, "video.post_shader", "", ""},
    {kPsxSettingsPackage, nullptr, "video.supersample", "1", "1"},
};
// The internal resolution when the host has no native resolution list.
static const PsxPresetValue kPsxPresetModResolution =
    {"psx.enhancement.internal-resolution", "internal-resolution", "resolution", "1x", "4x"};
// PGXP's explicit vertex/texture overrides are visual only. Keep the separate
// package off in both presets so its saved precise-culling option cannot change
// game math. Its CPU/culling options and the title's PGXP tuning are retained.
struct PsxPresetSwitch { const char *package, *feature; int enabled; };
static const PsxPresetSwitch kPsxPresetSwitches[] = {
    {"psx.enhancement.image-filtering", "image-filtering", 1},
    {"psx.enhancement.pgxp", "pgxp", 0},
    {"psx.enhancement.crt-filter", "crt-filter", 0},
};
static const PsxPresetSwitch kPsxPresetModResolutionSwitch =
    {"psx.enhancement.internal-resolution", "internal-resolution", 1};

/* The native resolution list's entry for a preset: native (1) for Faithful,
 * 1080 lines for Best (the list's nearest entry to the resolution package's 4x,
 * 960 lines). -1 when the host's list has no such entry. */
static int psx_native_resolution_index(LauncherModel* m, bool best) {
    const int want = best ? 1080 : 1;
    for (int i=0,n=launcher_model_internal_resolution_count(m); i<n; ++i)
        if (m->internal_resolution_values[i]==want) return i;
    return -1;
}
static bool psx_native_resolution_preset(LauncherModel* m) {
    return launcher_model_internal_resolution_offered(m) &&
           psx_native_resolution_index(m,false)>=0 && psx_native_resolution_index(m,true)>=0;
}
/* An entry the data allows: the option exists, is not overridden, and offers
 * both preset values. Entries the host does not offer are left out, so a
 * preset applies to what this host has instead of being disabled outright. */
static bool psx_preset_entry(LauncherModel* m, const PsxPresetValue& item,
                             RecompLauncherCModFeature& f, RecompLauncherCModOption& o) {
    return psx_find_option(m,item.package,item.feature,item.option,f,o) && !o.disabled &&
           psx_choice_has(m,f,o,item.faithful) && psx_choice_has(m,f,o,item.best);
}
template <typename Fn> static int psx_preset_each(LauncherModel* m, Fn&& fn) {
    int count=0;
    RecompLauncherCModFeature f{}; RecompLauncherCModOption o{};
    for (const auto& item:kPsxPreset)
        if (psx_preset_entry(m,item,f,o)) { ++count; if (!fn(item,f,o)) return -1; }
    if (!launcher_model_internal_resolution_offered(m) && psx_preset_entry(m,kPsxPresetModResolution,f,o)) {
        ++count; if (!fn(kPsxPresetModResolution,f,o)) return -1;
    }
    return count;
}
static bool psx_preset_ready(LauncherModel* m) {
    const auto* p=m->mods;
    return p && p->feature_option_get && p->feature_set_option && p->feature_enable &&
           launcher_model_can_open_mods(m) && m->has_renderer && m->has_frame_interp;
}
static bool psx_preset_available(LauncherModel* m) {
    if (!psx_preset_ready(m)) return false;
    return psx_native_resolution_preset(m) ||
           psx_preset_each(m,[](const PsxPresetValue&,const RecompLauncherCModFeature&,
                                const RecompLauncherCModOption&) { return true; })>0;
}
static int psx_current_preset(LauncherModel* m) {
    if (!psx_preset_available(m)) return -1;
    bool faithful=true,best=true;
    psx_preset_each(m,[&](const PsxPresetValue& item,const RecompLauncherCModFeature&,
                          const RecompLauncherCModOption& o) {
        faithful &= !strcmp(o.value,item.faithful);
        best &= !strcmp(o.value,item.best);
        return true;
    });
    if (psx_native_resolution_preset(m)) {
        const int i=launcher_model_internal_resolution_index(m);
        faithful &= i==psx_native_resolution_index(m,false);
        best &= i==psx_native_resolution_index(m,true);
    }
    auto check_switch=[&](const PsxPresetSwitch& item) {
        RecompLauncherCModFeature f{};
        if (!psx_feature(m,item.package,item.feature,f)) return;
        faithful &= f.enabled==item.enabled; best &= f.enabled==item.enabled;
    };
    for (const auto& item:kPsxPresetSwitches) check_switch(item);
    if (!launcher_model_internal_resolution_offered(m)) check_switch(kPsxPresetModResolutionSwitch);
    // Both are 4:3, with original motion and no adaptive widescreen or shader.
    if (m->s.renderer!=1 || m->s.aspect_index || m->s.widescreen ||
        m->s.frame_interp || m->s.frame_generation) return -1;
    return faithful ? 0 : best ? 1 : -1;
}
static bool psx_apply_preset(LauncherModel* m, bool best) {
    if (!psx_preset_available(m)) return false;
    const auto* p=m->mods;
    const int written=psx_preset_each(m,[&](const PsxPresetValue& item,const RecompLauncherCModFeature& f,
                                             const RecompLauncherCModOption& o) {
        return p->feature_set_option(p->ctx,f.package_id,f.id,o.id,best?item.best:item.faithful)!=0;
    });
    if (written<0) { mod_note_error(m); return false; } // Never label a partial/rejected write as a preset.
    auto set_switch=[&](const PsxPresetSwitch& item) {
        RecompLauncherCModFeature f{};
        return !psx_feature(m,item.package,item.feature,f) || p->feature_enable(p->ctx,item.package,item.feature,item.enabled);
    };
    for (const auto& item:kPsxPresetSwitches)
        if (!set_switch(item)) { mod_note_error(m); return false; }
    if (!launcher_model_internal_resolution_offered(m) && !set_switch(kPsxPresetModResolutionSwitch)) {
        mod_note_error(m); return false;
    }
    if (psx_native_resolution_preset(m))
        launcher_model_set_internal_resolution(m,psx_native_resolution_index(m,best));
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
            "Best: high resolution, corrected geometry/textures and higher-quality filtering. More GPU work; hardware may limit resolution.\n"
            "Both use 4:3 and keep original game timing and motion. A preset sets only the settings this game offers. "
            "Audio, bindings, cards and game-specific tuning are retained. "
            "Applies next launch; explicit command-line and environment overrides still win.");
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    ImGui::Spacing();
}

// ---- Rows, cards and headings ----------------------------------------------
// Page rows without a heading of their own keep the existing cards.
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
static bool psx_option_headed(const RecompLauncherCModFeature& f, const RecompLauncherCModOption& o) {
    return o.group[0] && strcmp(o.group,f.group) && strcmp(o.group,f.name);
}
/* The card an unheaded row belongs in. The PGXP page's own rows, and the PGXP
 * tuning rows of the Display page while a PGXP page exists, go to the PGXP tab. */
static const char* psx_option_card(LauncherModel* m, const RecompLauncherCModFeature& f,
                                   const RecompLauncherCModOption& o) {
    if (!strcmp(f.id,"pgxp")) return "PGXP";
    if (!strncmp(o.id,"video.pgxp_",11) && psx_has_pgxp_page(m)) return "PGXP tuning";
    return psx_option_group(o.id);
}
/* Draws (or, with draw false, counts) the rows of one page that belong to a
 * card (heading null) or to one heading. */
static int psx_runtime_rows(LauncherModel* m, const char* page, const char* card,
                            const char* heading, bool draw=true) {
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_feature(m,kPsxSettingsPackage,page,f)) return 0;
    int count=0;
    if (draw) { ImGui::PushID(f.package_id); ImGui::PushID(f.id); ImGui::BeginDisabled(!launcher_model_can_open_mods(m)); }
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (!p->feature_option_get(p->ctx,f.package_id,f.id,i,&o)) continue;
        const bool headed=psx_option_headed(f,o);
        if (heading ? !headed || strcmp(o.group,heading) : headed || strcmp(psx_option_card(m,f,o),card)) continue;
        ++count;
        if (draw) draw_mod_feature_option(m,f,o);
    }
    if (draw) { ImGui::EndDisabled(); ImGui::PopID(); ImGui::PopID(); }
    return count;
}
static void psx_runtime_group(LauncherModel* m,const char* page,const char* group) {
    psx_runtime_rows(m,page,group,nullptr);
}
// The page's own headings, in the order the host lists their first row.
static std::vector<std::string> psx_page_headings(LauncherModel* m, const char* page) {
    std::vector<std::string> out;
    const auto* p=m->mods; RecompLauncherCModFeature f{};
    if (!p || !p->feature_option_get || !psx_feature(m,kPsxSettingsPackage,page,f)) return out;
    for (int i=0;i<f.option_count;++i) {
        RecompLauncherCModOption o{};
        if (p->feature_option_get(p->ctx,f.package_id,f.id,i,&o) && psx_option_headed(f,o) &&
            std::find(out.begin(),out.end(),o.group)==out.end()) out.push_back(o.group);
    }
    return out;
}
static void psx_heading_card(LauncherModel* m, const char* page, const std::string& heading) {
    ImGui::PushID(page);
    if (begin_panel(heading.c_str())) {
        std::string upper=ui_text(heading.c_str());
        for (char& c:upper) c=(char)std::toupper((unsigned char)c);
        eyebrow_tracked(upper.c_str());
        psx_runtime_rows(m,page,nullptr,heading.c_str());
    }
    end_panel();
    ImGui::PopID();
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
            if (pgxp && ImGui::IsItemHovered()) ImGui::SetTooltip("The visual vertex and texture corrections work independently. This add-on also enables its saved CPU propagation and precise-culling options. Precise culling can change game math.");
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
/* The picture rows inside the Display card. A host that lists its own
 * resolutions gets a native Internal resolution row there instead of the
 * resolution package; a host with a PGXP page has the PGXP rows on that page. */
static void draw_psx_picture_controls(LauncherModel* m) {
    if (!launcher_model_internal_resolution_offered(m)) psx_package_group(m,"psx.enhancement.internal-resolution");
    psx_runtime_group(m,"display","Picture");
    if (!psx_has_pgxp_page(m)) psx_package_group(m,"psx.enhancement.pgxp");
}
static void psx_settings_card(LauncherModel* m,const LauncherTheme& th,const char* group) {
    if (begin_panel(group)) {
        if (!strcmp(group,"Display")) {
            draw_display_controls(m,th);
            if (psx_runtime_rows(m,"display",group,nullptr,false) &&
                ImGui::CollapsingHeader("PGXP tuning",ImGuiTreeNodeFlags_DefaultOpen)) psx_runtime_group(m,"display",group);
        } else if (!strcmp(group,"Image")) {
            eyebrow("IMAGE QUALITY");
            psx_package_group(m,"psx.enhancement.image-filtering");
            psx_runtime_group(m,"display",group);
        } else if (!strcmp(group,"Effects")) {
            eyebrow("EFFECTS AND PRESENTATION");
            psx_runtime_group(m,"display",group);
            psx_package_group(m,"psx.enhancement.crt-filter");
        } else if (!strcmp(group,"PGXP")) {
            eyebrow("PGXP");
            psx_runtime_group(m,"pgxp",group);
            if (psx_runtime_rows(m,"display","PGXP tuning",nullptr,false) &&
                ImGui::CollapsingHeader("PGXP tuning",ImGuiTreeNodeFlags_DefaultOpen)) psx_runtime_group(m,"display","PGXP tuning");
        } else if (!strcmp(group,"PGXP CPU")) {
            eyebrow("PGXP CPU AND CULLING");
            psx_package_group(m,"psx.enhancement.pgxp");
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
enum { PSX_SETTINGS_GENERAL=0, PSX_SETTINGS_DISPLAY=1, PSX_SETTINGS_PGXP=2 };
/* One Settings tab: its fixed cards, then a card for each heading the host
 * gives the page. Three columns on a wide page, two, or one. */
static void draw_psx_settings_page(LauncherModel* m,const LauncherTheme& th,int page) {
    static const char* const kCards[][3]={{"System","Audio","Input"},{"Display","Image","Effects"},{"PGXP","PGXP CPU",nullptr}};
    static const char* const kPages[]={"general","display","pgxp"};
    if (page<PSX_SETTINGS_GENERAL || page>PSX_SETTINGS_PGXP) return;
    if (page==PSX_SETTINGS_DISPLAY) draw_psx_presets(m);
    const float width=ImGui::GetContentRegionAvail().x;
    const int columns=width>=px(1500)?3:width>=px(840)?2:1;
    const auto headings=psx_page_headings(m,kPages[page]);
    g_psx_compact_settings=true;
    if (ImGui::BeginTable("psx_settings_groups",columns,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); psx_settings_card(m,th,kCards[page][0]);
        if (columns>1) ImGui::TableNextColumn();
        psx_settings_card(m,th,kCards[page][1]);
        if (kCards[page][2]) {
            // With two columns the third card stacks under the second.
            if (columns==3 || columns==1) ImGui::TableNextColumn();
            psx_settings_card(m,th,kCards[page][2]);
        }
        for (const auto& heading:headings) { ImGui::TableNextColumn(); psx_heading_card(m,kPages[page],heading); }
        ImGui::EndTable();
    }
    g_psx_compact_settings=false;
}
