// FIXME: extract absolute units -> em

#include "ConfigWizard_private.hpp"

#include <algorithm>
#include <numeric>
#include <utility>
#include <boost/unordered_map.hpp>
#include <stdexcept>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/dcclient.h>
#include <wx/statbmp.h>
#include <wx/checkbox.h>
#include <wx/statline.h>
#include <wx/dataview.h>
#include <wx/notebook.h>
#include <wx/display.h>
#include <wx/filefn.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/Config.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "slic3r/Config/Snapshot.hpp"
#include "slic3r/Utils/PresetUpdater.hpp"

#if defined(__linux__) && defined(__WXGTK3__)
#define wxLinux_gtk3 true
#else
#define wxLinux_gtk3 false
#endif //defined(__linux__) && defined(__WXGTK3__)

namespace Slic3r {
namespace GUI {

#define MAIN_VENDOR "None"

using Config::Snapshot;
using Config::SnapshotDB;


// Configuration data structures extensions needed for the wizard

bool Bundle::load(fs::path source_path, bool ais_in_resources, bool ais_prusa_bundle)
{
    this->preset_bundle = std::make_unique<PresetBundle>();
    this->is_in_resources = ais_in_resources;
    this->is_prusa_bundle = ais_prusa_bundle;

    std::string path_string = source_path.string();
    size_t presets_loaded = preset_bundle->load_configbundle(path_string, PresetBundle::LOAD_CFGBNDLE_SYSTEM);
    auto first_vendor = preset_bundle->vendors.begin();
    if (first_vendor == preset_bundle->vendors.end()) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: No vendor information defined, cannot install.") % path_string;
        return false;
    }
    if (presets_loaded == 0) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: No profile loaded.") % path_string;
        return false;
    } 

    BOOST_LOG_TRIVIAL(trace) << boost::format("Vendor bundle: `%1%`: %2% profiles loaded.") % path_string % presets_loaded;
    this->vendor_profile = &first_vendor->second;
    return true;
}

Bundle::Bundle(Bundle &&other)
    : preset_bundle(std::move(other.preset_bundle))
    , vendor_profile(other.vendor_profile)
    , is_in_resources(other.is_in_resources)
    , is_prusa_bundle(other.is_prusa_bundle)
{
    other.vendor_profile = nullptr;
}

BundleMap BundleMap::load()
{
    BundleMap res;

    const auto vendor_dir = (boost::filesystem::path(Slic3r::data_dir()) / "vendor").make_preferred();
    const auto rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();

    // commented prusa bundle mandatory check at startup
    //auto prusa_bundle_path = (vendor_dir / PresetBundle::PRUSA_BUNDLE).replace_extension(".ini");
    //auto prusa_bundle_rsrc = false;
    //if (! boost::filesystem::exists(prusa_bundle_path)) {
    //    prusa_bundle_path = (rsrc_vendor_dir / PresetBundle::PRUSA_BUNDLE).replace_extension(".ini");
    //    prusa_bundle_rsrc = true;
    //}
    //{
    //    Bundle prusa_bundle;
    //    if (prusa_bundle.load(std::move(prusa_bundle_path), prusa_bundle_rsrc, true))
    //        res.emplace(PresetBundle::PRUSA_BUNDLE, std::move(prusa_bundle)); 
    //}

    // Load the other bundles in the datadir/vendor directory
    // and then additionally from resources/profiles.
    bool is_in_resources = false;
    for (auto dir : { &vendor_dir, &rsrc_vendor_dir }) {
        for (const auto &dir_entry : boost::filesystem::directory_iterator(*dir)) {
            if (Slic3r::is_ini_file(dir_entry)) {
                std::string id = dir_entry.path().stem().string();  // stem() = filename() without the trailing ".ini" part

                // Don't load this bundle if we've already loaded it.
                if (res.find(id) != res.end()) { continue; }

                Bundle bundle;
                if (bundle.load(dir_entry.path(), is_in_resources))
                    res.emplace(std::move(id), std::move(bundle));
            }
        }

        is_in_resources = true;
    }

    return res;
}

Bundle& BundleMap::prusa_bundle()
{
    auto it = find(PresetBundle::PRUSA_BUNDLE);
    if (it == end()) {
        throw Slic3r::RuntimeError("ConfigWizard: Internal error in BundleMap: PRUSA_BUNDLE not loaded");
    }

    return it->second;
}

const Bundle& BundleMap::prusa_bundle() const
{
    return const_cast<BundleMap*>(this)->prusa_bundle();
}


// Printer model picker GUI control

struct PrinterPickerEvent : public wxEvent
{
    std::string vendor_id;
    std::string model_id;
    std::string variant_name;
    bool enable;

    PrinterPickerEvent(wxEventType eventType, int winid, std::string vendor_id, std::string model_id, std::string variant_name, bool enable)
        : wxEvent(winid, eventType)
        , vendor_id(std::move(vendor_id))
        , model_id(std::move(model_id))
        , variant_name(std::move(variant_name))
        , enable(enable)
    {}

    virtual wxEvent *Clone() const
    {
        return new PrinterPickerEvent(*this);
    }
};

wxDEFINE_EVENT(EVT_PRINTER_PICK, PrinterPickerEvent);

const std::string PrinterPicker::PRINTER_PLACEHOLDER = "printer_placeholder.png";

PrinterPicker::PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig, const ModelFilter &filter)
    : wxPanel(parent)
    , vendor_id(vendor.id)
    , width(0)
{
    const std::vector<VendorProfile::PrinterModel> &models = vendor.models;

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    const auto font_title = GetFont().MakeBold().Scaled(1.3f);
    const auto font_name = GetFont().MakeBold();
    const auto font_alt_nozzle = GetFont().Scaled(0.9f);

    // wxGrid appends widgets by rows, but we need to construct them in columns.
    // These vectors are used to hold the elements so that they can be appended in the right order.
    std::vector<wxStaticText*> titles;
    std::vector<wxStaticBitmap*> bitmaps;
    std::vector<wxPanel*> variants_panels;

    int max_row_width = 0;
    int current_row_width = 0;

    bool is_variants = false;

    for (const VendorProfile::PrinterModel &model : models) {
        if (! filter(model)) { continue; }

        wxBitmap bitmap;
        int bitmap_width = 0;
        auto load_bitmap = [](const wxString& bitmap_file, wxBitmap& bitmap, int& bitmap_width)->bool {
        if (wxFileExists(bitmap_file)) {
            bitmap.LoadFile(bitmap_file, wxBITMAP_TYPE_PNG);
            bitmap_width = bitmap.GetWidth();
                return true;
            }
            return false;
        };
        std::string icon_path = (model.thumbnail.empty()) ?
            ("/" + model.id + "_thumbnail.png") :
            ("/" + model.thumbnail);
        if (!load_bitmap(GUI::from_u8(Slic3r::data_dir() + "/vendor/" + vendor.id + icon_path), bitmap, bitmap_width)) {
            if (!load_bitmap(GUI::from_u8(Slic3r::resources_dir() + "/profiles/" + vendor.id + icon_path), bitmap, bitmap_width)) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("Can't find bitmap file `%1%` for vendor `%2%`, printer `%3%`, using placeholder icon instead")
                    % (Slic3r::resources_dir() + "/profiles/" + vendor.id + "/" + model.id + "_thumbnail.png")
                % vendor.id
                % model.id;
                load_bitmap(Slic3r::var(PRINTER_PLACEHOLDER), bitmap, bitmap_width);
        }
        }
        auto *title = new wxStaticText(this, wxID_ANY, from_u8(model.name), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
        title->SetFont(font_name);
        const int wrap_width = std::max((int)MODEL_MIN_WRAP, bitmap_width);
        title->Wrap(wrap_width);

        current_row_width += wrap_width;
        if (titles.size() % max_cols == max_cols - 1) {
            max_row_width = std::max(max_row_width, current_row_width);
            current_row_width = 0;
        }

        titles.push_back(title);

        auto *bitmap_widget = new wxStaticBitmap(this, wxID_ANY, bitmap);
        bitmaps.push_back(bitmap_widget);

        auto *variants_panel = new wxPanel(this);
        auto *variants_sizer = new wxBoxSizer(wxVERTICAL);
        variants_panel->SetSizer(variants_sizer);
        const auto model_id = model.id;

        for (size_t i = 0; i < model.variants.size(); i++) {
            const auto &variant = model.variants[i];

            const auto label = model.technology == ptFFF
                ? from_u8((boost::format("%1% %2% %3%") % variant.name % _utf8(L("mm")) % _utf8(L("nozzle"))).str())
                : from_u8(model.name);

            if (i == 1) {
                auto *alt_label = new wxStaticText(variants_panel, wxID_ANY, _L("Alternate nozzles:"));
                alt_label->SetFont(font_alt_nozzle);
                variants_sizer->Add(alt_label, 0, wxBOTTOM, 3);
                is_variants = true;
            }

            auto *cbox = new Checkbox(variants_panel, label, model_id, variant.name);
            i == 0 ? cboxes.push_back(cbox) : cboxes_alt.push_back(cbox);

            const bool enabled = appconfig.get_variant(vendor.id, model_id, variant.name);
            cbox->SetValue(enabled);

            variants_sizer->Add(cbox, 0, wxBOTTOM, 3);

            cbox->Bind(wxEVT_CHECKBOX, [this, cbox](wxCommandEvent &event) {
                on_checkbox(cbox, event.IsChecked());
            });
        }

        variants_panels.push_back(variants_panel);
    }

    width = std::max(max_row_width, current_row_width);

    const size_t cols = std::min(max_cols, titles.size());

    auto *printer_grid = new wxFlexGridSizer(cols, 0, 20);
    printer_grid->SetFlexibleDirection(wxVERTICAL | wxHORIZONTAL);

    if (titles.size() > 0) {
        const size_t odd_items = titles.size() % cols;

        for (size_t i = 0; i < titles.size() - odd_items; i += cols) {
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(bitmaps[j], 0, wxBOTTOM, 20); }
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(titles[j], 0, wxBOTTOM, 3); }
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(variants_panels[j]); }

            // Add separator space to multiliners
            if (titles.size() > cols) {
                for (size_t j = i; j < i + cols; j++) { printer_grid->Add(1, 30); }
            }
        }
        if (odd_items > 0) {
            const size_t rem = titles.size() - odd_items;

            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(bitmaps[i], 0, wxBOTTOM, 20); }
            for (size_t i = 0; i < cols - odd_items; i++) { printer_grid->AddSpacer(1); }
            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(titles[i], 0, wxBOTTOM, 3); }
            for (size_t i = 0; i < cols - odd_items; i++) { printer_grid->AddSpacer(1); }
            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(variants_panels[i]); }
        }
    }

    auto *title_sizer = new wxBoxSizer(wxHORIZONTAL);
    if (! title.IsEmpty()) {
        auto *title_widget = new wxStaticText(this, wxID_ANY, title);
        title_widget->SetFont(font_title);
        title_sizer->Add(title_widget);
    }
    title_sizer->AddStretchSpacer();

    if (/*titles.size() > 1*/is_variants) {
        // It only makes sense to add the All / None buttons if there's multiple printers

        auto *sel_all_std = new wxButton(this, wxID_ANY, titles.size() > 1 ? _L("All standard") : _L("Standard"));
        auto *sel_all = new wxButton(this, wxID_ANY, _L("All"));
        auto *sel_none = new wxButton(this, wxID_ANY, _L("None"));
        sel_all_std->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &event) { this->select_all(true, false); });
        sel_all->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &event) { this->select_all(true, true); });
        sel_none->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &event) { this->select_all(false); });
        title_sizer->Add(sel_all_std, 0, wxRIGHT, BTN_SPACING);
        title_sizer->Add(sel_all, 0, wxRIGHT, BTN_SPACING);
        title_sizer->Add(sel_none);

        // fill button indexes used later for buttons rescaling
        m_button_indexes = { sel_all_std->GetId(), sel_all->GetId(), sel_none->GetId() };
    }

    sizer->Add(title_sizer, 0, wxEXPAND | wxBOTTOM, BTN_SPACING);
    sizer->Add(printer_grid);

    SetSizer(sizer);
}

PrinterPicker::PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig)
    : PrinterPicker(parent, vendor, std::move(title), max_cols, appconfig, [](const VendorProfile::PrinterModel&) { return true; })
{}

void PrinterPicker::select_all(bool select, bool alternates)
{
    for (const auto &cb : cboxes) {
        if (cb->GetValue() != select) {
            cb->SetValue(select);
            on_checkbox(cb, select);
        }
    }

    if (! select) { alternates = false; }

    for (const auto &cb : cboxes_alt) {
        if (cb->GetValue() != alternates) {
            cb->SetValue(alternates);
            on_checkbox(cb, alternates);
        }
    }
}

void PrinterPicker::select_one(size_t i, bool select)
{
    if (i < cboxes.size() && cboxes[i]->GetValue() != select) {
        cboxes[i]->SetValue(select);
        on_checkbox(cboxes[i], select);
    }
}

bool PrinterPicker::any_selected() const
{
    for (const auto &cb : cboxes) {
        if (cb->GetValue()) { return true; }
    }

    for (const auto &cb : cboxes_alt) {
        if (cb->GetValue()) { return true; }
    }

    return false;
}

std::set<std::string> PrinterPicker::get_selected_models() const 
{
    std::set<std::string> ret_set;

    for (const auto& cb : cboxes)
        if (cb->GetValue())
            ret_set.emplace(cb->model);

    for (const auto& cb : cboxes_alt)
        if (cb->GetValue())
            ret_set.emplace(cb->model);

    return ret_set;
}

void PrinterPicker::on_checkbox(const Checkbox *cbox, bool checked)
{
    PrinterPickerEvent evt(EVT_PRINTER_PICK, GetId(), vendor_id, cbox->model, cbox->variant, checked);
    AddPendingEvent(evt);
}


// Wizard page base

ConfigWizardPage::ConfigWizardPage(ConfigWizard *parent, wxString title, wxString shortname, unsigned indent)
    : wxPanel(parent->p->hscroll)
    , parent(parent)
    , shortname(std::move(shortname))
    , indent(indent)
{
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto *text = new wxStaticText(this, wxID_ANY, std::move(title), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    const auto font = GetFont().MakeBold().Scaled(1.5);
    text->SetFont(font);
    sizer->Add(text, 0, wxALIGN_LEFT, 0);
    sizer->AddSpacer(10);

    content = new wxBoxSizer(wxVERTICAL);
    sizer->Add(content, 1, wxEXPAND);

    SetSizer(sizer);

    // There is strange layout on Linux with GTK3, 
    // see https://github.com/prusa3d/PrusaSlicer/issues/5103 and https://github.com/prusa3d/PrusaSlicer/issues/4861
    // So, non-active pages will be hidden later, on wxEVT_SHOW, after completed Layout() for all pages 
    if (!wxLinux_gtk3)
        this->Hide();

    Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        this->Layout();
        event.Skip();
    });
}

ConfigWizardPage::~ConfigWizardPage() {}

wxStaticText* ConfigWizardPage::append_text(wxString text)
{
    auto *widget = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->Wrap(WRAP_WIDTH);
    widget->SetMinSize(wxSize(WRAP_WIDTH, -1));
    append(widget);
    return widget;
}

void ConfigWizardPage::append_spacer(int space)
{
    // FIXME: scaling
    content->AddSpacer(space);
}


// Wizard pages

PageWelcome::PageWelcome(ConfigWizard *parent)
    : ConfigWizardPage(parent, from_u8((boost::format(
#ifdef __APPLE__
            _utf8(L("Welcome to the %s Configuration Assistant"))
#else
            _utf8(L("Welcome to the %s Configuration Wizard"))
#endif
            ) % SLIC3R_APP_NAME).str()), _L("Welcome"))
    , welcome_text(append_text(from_u8((boost::format(
        _utf8(L("Hello, welcome to %s! This %s helps you with the initial configuration; just a few settings and you will be ready to print.")))
        % SLIC3R_APP_NAME
        % _utf8(ConfigWizard::name())).str())
    ))
    , cbox_reset(append(
        new wxCheckBox(this, wxID_ANY, _L("Remove user profiles (a snapshot will be taken beforehand)"))
    ))
{
    welcome_text->Hide();
    cbox_reset->Hide();
}

void PageWelcome::set_run_reason(ConfigWizard::RunReason run_reason)
{
    const bool data_empty = run_reason == ConfigWizard::RR_DATA_EMPTY;
    welcome_text->Show(data_empty);
    cbox_reset->Show(!data_empty);
}


PagePrinters::PagePrinters(ConfigWizard *parent,
    wxString title,
    wxString shortname,
    const VendorProfile &vendor,
    unsigned indent,
    Technology technology)
    : ConfigWizardPage(parent, std::move(title), std::move(shortname), indent)
    , technology((Technology)(uint8_t)technology)
    , install(false)   // only used for 3rd party vendors
{
    enum {
        COL_SIZE = 200,
    };

    AppConfig *appconfig = &this->wizard_p()->appconfig_new;

    const t_config_option_keys families = vendor.families();
    for (const std::string &family : families) {
        const auto filter = [&](const VendorProfile::PrinterModel &model) {
            return (model.technology == technology)
                && model.family == family;
        };

        if (std::find_if(vendor.models.begin(), vendor.models.end(), filter) == vendor.models.end()) {
            continue;
        }

        const auto picker_title = family.empty() ? wxString() : from_u8((boost::format(_utf8(L("%s Family"))) % family).str());
        uint8_t max_cols = MAX_COLS;
        if (vendor.family_2_line_size.find(family) != vendor.family_2_line_size.end())
            max_cols = vendor.family_2_line_size.at(family);
        auto *picker = new PrinterPicker(this, vendor, picker_title, max_cols, *appconfig, filter);

        picker->Bind(EVT_PRINTER_PICK, [this, appconfig](const PrinterPickerEvent &evt) {
            appconfig->set_variant(evt.vendor_id, evt.model_id, evt.variant_name, evt.enable);
            wizard_p()->on_printer_pick(this, evt);
        });

        append(new wxStaticLine(this));

        append(picker);
        printer_pickers.push_back(picker);
    }
}

void PagePrinters::select_all(bool select, bool alternates)
{
    for (auto picker : printer_pickers) {
        picker->select_all(select, alternates);
    }
}

int PagePrinters::get_width() const
{
    return std::accumulate(printer_pickers.begin(), printer_pickers.end(), 0,
        [](int acc, const PrinterPicker *picker) { return std::max(acc, picker->get_width()); });
}

bool PagePrinters::any_selected() const
{
    for (const auto *picker : printer_pickers) {
        if (picker->any_selected()) { return true; }
    }

    return false;
}

std::set<std::string> PagePrinters::get_selected_models()
{
    std::set<std::string> ret_set;

    for (const auto *picker : printer_pickers)
    {
        std::set<std::string> tmp_models = picker->get_selected_models();
        ret_set.insert(tmp_models.begin(), tmp_models.end());
    }

    return ret_set;
}

void PagePrinters::set_run_reason(ConfigWizard::RunReason run_reason)
{
#ifdef ALLOW_PRUSA_FIRST
    if (technology == T_FFF
        && (run_reason == ConfigWizard::RR_DATA_EMPTY || run_reason == ConfigWizard::RR_DATA_LEGACY)
        && printer_pickers.size() > 0 
        && printer_pickers[0]->vendor_id == PresetBundle::PRUSA_BUNDLE) {
        printer_pickers[0]->select_one(0, true);
    }
#else
    //didn't select the first prusa even if it's the first start: let the user choose
#endif
}


const std::string PageMaterials::EMPTY;

PageMaterials::PageMaterials(ConfigWizard *parent, Materials *materials, wxString title, wxString shortname, wxString list1name)
    : ConfigWizardPage(parent, std::move(title), std::move(shortname))
    , materials(materials)
	, list_printer(new  StringList(this, wxLB_MULTIPLE))
    , list_type(new StringList(this))
    , list_vendor(new StringList(this))
    , list_profile(new PresetList(this))
{
    append_spacer(VERTICAL_SPACING);

    const int em = parent->em_unit();
    const int list_h = 30*em;


	list_printer->SetMinSize(wxSize(23*em, list_h));
    list_type->SetMinSize(wxSize(8*em, list_h));
    list_vendor->SetMinSize(wxSize(13*em, list_h));
    list_profile->SetMinSize(wxSize(23*em, list_h));



    grid = new wxFlexGridSizer(4, em/2, em);
    grid->AddGrowableCol(3, 1);
    grid->AddGrowableRow(1, 1);

	grid->Add(new wxStaticText(this, wxID_ANY, _L("Printer:")));
    grid->Add(new wxStaticText(this, wxID_ANY, list1name));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Vendor:")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Profile:")));

	grid->Add(list_printer, 0, wxEXPAND);
    grid->Add(list_type, 0, wxEXPAND);
    grid->Add(list_vendor, 0, wxEXPAND);
    grid->Add(list_profile, 1, wxEXPAND);

    auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *sel_all = new wxButton(this, wxID_ANY, _L("All"));
    auto *sel_none = new wxButton(this, wxID_ANY, _L("None"));
    btn_sizer->Add(sel_all, 0, wxRIGHT, em / 2);
    btn_sizer->Add(sel_none);


    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(btn_sizer, 0, wxALIGN_RIGHT);

    append(grid, 1, wxEXPAND);

    append_spacer(VERTICAL_SPACING);

    html_window = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition,
        wxSize(60 * em, 20 * em), wxHW_SCROLLBAR_AUTO);
    append(html_window, 0, wxEXPAND);

	list_printer->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& evt) {
		update_lists(evt.GetInt(), list_type->GetSelection(), list_vendor->GetSelection());
		});
    list_type->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        update_lists(list_printer->GetSelection(), list_type->GetSelection(), list_vendor->GetSelection());
    });
    list_vendor->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        update_lists(list_printer->GetSelection(), list_type->GetSelection(), list_vendor->GetSelection());
    });

    list_profile->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent &evt) { select_material(evt.GetInt()); });
    list_profile->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& evt) { on_material_highlighted(evt.GetInt()); });

    sel_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { select_all(true); });
    sel_none->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { select_all(false); });
    /*
    Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {on_paint();});

    list_profile->Bind(wxEVT_MOTION, [this](wxMouseEvent& evt) { on_mouse_move_on_profiles(evt); });
    list_profile->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& evt) { on_mouse_enter_profiles(evt); });
    list_profile->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) { on_mouse_leave_profiles(evt); });
    */
    reload_presets();
    set_compatible_printers_html_window(std::vector<std::string>(), false);
}
void PageMaterials::on_paint()
{
}
void PageMaterials::on_mouse_move_on_profiles(wxMouseEvent& evt)
{
    const wxClientDC dc(list_profile);
    const wxPoint pos = evt.GetLogicalPosition(dc);
    int item = list_profile->HitTest(pos);
    on_material_hovered(item);
}
void PageMaterials::on_mouse_enter_profiles(wxMouseEvent& evt)
{}
void PageMaterials::on_mouse_leave_profiles(wxMouseEvent& evt)
{
    on_material_hovered(-1);
}
void PageMaterials::reload_presets()
{
    clear();

	list_printer->append(_L("(All)"), &EMPTY);
    //list_printer->SetLabelMarkup("<b>bald</b>");
	for (const Preset* printer : materials->printers) {
		list_printer->append(printer->name, &printer->name);
	}
    sort_list_data(list_printer, true, false);
    if (list_printer->GetCount() > 0) {
        list_printer->SetSelection(0);
		sel_printer_count_prev = wxNOT_FOUND;
        sel_printer_item_prev = wxNOT_FOUND;
        sel_type_prev = wxNOT_FOUND;
        sel_vendor_prev = wxNOT_FOUND;
        update_lists(0, 0, 0);
    }

    presets_loaded = true;
}

void PageMaterials::set_compatible_printers_html_window(const std::vector<std::string>& printer_names, bool all_printers)
{
    const auto bgr_clr = 
#if defined(__APPLE__)
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
#else
        wxSystemSettings::GetColour(wxSYS_COLOUR_MENU);
#endif
    const auto bgr_clr_str = wxString::Format(wxT("#%02X%02X%02X"), bgr_clr.Red(), bgr_clr.Green(), bgr_clr.Blue());
    const auto text_clr = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const auto text_clr_str = wxString::Format(wxT("#%02X%02X%02X"), text_clr.Red(), text_clr.Green(), text_clr.Blue());
    wxString first_line = _L("Filaments marked with <b>*</b> are <b>not</b> compatible with some installed printers.");
    wxString text;
    if (all_printers) {
        wxString second_line = _L("All installed printers are compatible with the selected filament.");
        text = wxString::Format(
            "<html>"
            "<style>"
            "table{border-spacing: 1px;}"
            "</style>"
            "<body bgcolor= %s>"
            "<font color=%s>"
            "<font size=\"3\">"
            "%s<br /><br />%s"
            "</font>"
            "</font>"
            "</body>"
            "</html>"
            , bgr_clr_str
            , text_clr_str
            , first_line
            , second_line
            );
    } else {
        wxString second_line = _L("Only the following installed printers are compatible with the selected filament:");
        text = wxString::Format(
            "<html>"
            "<style>"
            "table{border-spacing: 1px;}"
            "</style>"
            "<body bgcolor= %s>"
            "<font color=%s>"
            "<font size=\"3\">"
            "%s<br /><br />%s"
            "<table>"
            "<tr>"
            , bgr_clr_str
            , text_clr_str
            , first_line
            , second_line);
        for (int i = 0; i < printer_names.size(); ++i)
        {
            text += wxString::Format("<td>%s</td>", boost::nowide::widen(printer_names[i]));
            if (i % 3 == 2) {
                text += wxString::Format(
                    "</tr>"
                    "<tr>");
            }
        }
        text += wxString::Format(
            "</tr>"
            "</table>"
            "</font>"
            "</font>"
            "</body>"
            "</html>"
        );
    }
   
    wxFont font = get_default_font_for_dpi(this, get_dpi_for_window(this));
    const int fs = font.GetPointSize();
    int size[] = { fs,fs,fs,fs,fs,fs,fs };
    html_window->SetFonts(font.GetFaceName(), font.GetFaceName(), size);
    html_window->SetPage(text);
}

void PageMaterials::clear_compatible_printers_label()
{
    set_compatible_printers_html_window(std::vector<std::string>(), false);
}

void PageMaterials::on_material_hovered(int sel_material)
{

}

void PageMaterials::on_material_highlighted(int sel_material)
{
    if (sel_material == last_hovered_item)
        return;
    if (sel_material == -1) {
        clear_compatible_printers_label();
        return;
    }
    last_hovered_item = sel_material;
    std::vector<std::string> tabs;
    tabs.push_back(std::string());
    tabs.push_back(std::string());
    tabs.push_back(std::string());
    //selected material string
    std::string material_name = list_profile->get_data(sel_material);
    // get material preset
    const std::vector<const Preset*> matching_materials = materials->get_presets_by_alias(material_name);
    if (matching_materials.empty())
    {
        clear_compatible_printers_label();
        return;
    }
    //find matching printers
    std::vector<std::string> names;
    for (const Preset* printer : materials->printers) {
        for (const Preset* material : matching_materials) {
            if (is_compatible_with_printer(PresetWithVendorProfile(*material, material->vendor), PresetWithVendorProfile(*printer, printer->vendor))) {
                names.push_back(printer->name);
                break;
            }
        }
    }
    set_compatible_printers_html_window(names, names.size() == materials->printers.size());
}

void PageMaterials::update_lists(int sel_printer, int sel_type, int sel_vendor)
{
	wxWindowUpdateLocker freeze_guard(this);
	(void)freeze_guard;

	wxArrayInt sel_printers;
	int sel_printers_count = list_printer->GetSelections(sel_printers);

	if (sel_printers_count != sel_printer_count_prev || (sel_printers_count == 1 && sel_printer_item_prev != sel_printer && sel_printer != -1)) {
        // Refresh type list
		list_type->Clear();
		list_type->append(_L("(All)"), &EMPTY);
		if (sel_printers_count > 0) {
            // If all is selected with other printers
            // unselect "all" or all printers depending on last value
            if (sel_printers[0] == 0 && sel_printers_count > 1) {
                if (sel_printer == 0) {
                    list_printer->SetSelection(wxNOT_FOUND);
                    list_printer->SetSelection(0);
                } else {
                    list_printer->SetSelection(0, false);
                    sel_printers_count = list_printer->GetSelections(sel_printers);
                }
            }
			if (sel_printers[0] != 0) {
				for (size_t i = 0; i < sel_printers_count; i++) {
					const std::string& printer_name = list_printer->get_data(sel_printers[i]);
					const Preset* printer = nullptr;
					for (const Preset* it : materials->printers) {
						if (it->name == printer_name) {
							printer = it;
							break;
						}
					}
					materials->filter_presets(printer, EMPTY, EMPTY, [this](const Preset* p) {
						const std::string& type = this->materials->get_type(p);
						if (list_type->find(type) == wxNOT_FOUND) {
							list_type->append(type, &type);
						}
						});
				}
			} else {
                //clear selection except "ALL"
                list_printer->SetSelection(wxNOT_FOUND);
                list_printer->SetSelection(0);
                sel_printers_count = list_printer->GetSelections(sel_printers);

				materials->filter_presets(nullptr, EMPTY, EMPTY, [this](const Preset* p) {
					const std::string& type = this->materials->get_type(p);
					if (list_type->find(type) == wxNOT_FOUND) {
						list_type->append(type, &type);
					}
					});
			}
            sort_list_data(list_type, true, true);
		}

		sel_printer_count_prev = sel_printers_count;
        sel_printer_item_prev = sel_printer;
		sel_type = 0;
		sel_type_prev = wxNOT_FOUND;
		list_type->SetSelection(sel_type);
		list_profile->Clear();
	}
	
	if (sel_type != sel_type_prev) {
		// Refresh vendor list

		// XXX: The vendor list is created with quadratic complexity here,
		// but the number of vendors is going to be very small this shouldn't be a problem.

		list_vendor->Clear();
		list_vendor->append(_L("(All)"), &EMPTY);
		if (sel_printers_count != 0 && sel_type != wxNOT_FOUND) {
			const std::string& type = list_type->get_data(sel_type);
			// find printer preset
			for (size_t i = 0; i < sel_printers_count; i++) {
				const std::string& printer_name = list_printer->get_data(sel_printers[i]);
				const Preset* printer = nullptr;
				for (const Preset* it : materials->printers) {
					if (it->name == printer_name) {
						printer = it;
						break;
					}
				}
				materials->filter_presets(printer, type, EMPTY, [this](const Preset* p) {
					const std::string& vendor = this->materials->get_vendor(p);
					if (list_vendor->find(vendor) == wxNOT_FOUND) {
						list_vendor->append(vendor, &vendor);
					}
					});
			}
            sort_list_data(list_vendor, true, false);
		}

		sel_type_prev = sel_type;
		sel_vendor = 0;
		sel_vendor_prev = wxNOT_FOUND;
		list_vendor->SetSelection(sel_vendor);
		list_profile->Clear();
	}
         
	if (sel_vendor != sel_vendor_prev) {
		// Refresh material list

		list_profile->Clear();
        clear_compatible_printers_label();
		if (sel_printers_count != 0 && sel_type != wxNOT_FOUND && sel_vendor != wxNOT_FOUND) {
			const std::string& type = list_type->get_data(sel_type);
			const std::string& vendor = list_vendor->get_data(sel_vendor);
			// finst printer preset
            std::vector<ProfilePrintData> to_list;
			for (size_t i = 0; i < sel_printers_count; i++) {
				const std::string& printer_name = list_printer->get_data(sel_printers[i]);
				const Preset* printer = nullptr;
				for (const Preset* it : materials->printers) {
					if (it->name == printer_name) {
						printer = it;
						break;
					}
				}

				materials->filter_presets(printer, type, vendor, [this, &to_list](const Preset* p) {
					bool was_checked = false;
					//size_t printer_counter = materials->get_printer_counter(p);
					int cur_i = list_profile->find(p->alias);
                    bool emplace_to_to_list = false;
					if (cur_i == wxNOT_FOUND) {
						cur_i = list_profile->append(p->alias + (materials->get_omnipresent(p) ? "" : " *"), &p->alias);
                        emplace_to_to_list = true;
                    } else
						was_checked = list_profile->IsChecked(cur_i);

					const std::string& section = materials->appconfig_section();

					const bool checked = wizard_p()->appconfig_new.has(section, p->name);
					list_profile->Check(cur_i, checked || was_checked);
                    if (emplace_to_to_list) 
                        to_list.emplace_back(p->alias, materials->get_omnipresent(p), checked || was_checked);

					/* Update preset selection in config.
					 * If one preset from aliases bundle is selected,
					 * than mark all presets with this aliases as selected
					 * */
					if (checked && !was_checked)
						wizard_p()->update_presets_in_config(section, p->alias, true);
					else if (!checked && was_checked)
						wizard_p()->appconfig_new.set(section, p->name, "1");
					});
			}
            sort_list_data(list_profile, to_list);
		}

		sel_vendor_prev = sel_vendor;
	}
}

void PageMaterials::sort_list_data(StringList* list, bool add_All_item, bool material_type_ordering)
{
// get data from list
// sort data
// first should be <all>
// then prusa profiles
// then the rest
// in alphabetical order
    
    std::vector<std::reference_wrapper<const std::string>> prusa_profiles;
    std::vector<std::reference_wrapper<const std::string>> other_profiles;
    for (int i = 0 ; i < list->size(); ++i) {
        const std::string& data = list->get_data(i);
        if (data == EMPTY) // do not sort <all> item
            continue;
        if (!material_type_ordering && data.find("Prusa") != std::string::npos)
            prusa_profiles.push_back(data);
        else 
            other_profiles.push_back(data);
    }
    if(material_type_ordering) {
        
        const ConfigOptionDef* def = print_config_def.get("filament_type");
        std::vector<std::string>enum_values = def->enum_values;
        int end_of_sorted = 0;
        for (size_t vals = 0; vals < enum_values.size(); vals++) {
            for (size_t profs = end_of_sorted; profs < other_profiles.size(); profs++)
            {
                // find instead compare because PET vs PETG
                if (other_profiles[profs].get().find(enum_values[vals]) != std::string::npos) {
                    //swap
                    if(profs != end_of_sorted) {
                        std::reference_wrapper<const std::string> aux = other_profiles[end_of_sorted];
                        other_profiles[end_of_sorted] = other_profiles[profs];
                        other_profiles[profs] = aux;
                    }
                    end_of_sorted++;
                    break;
                }
            }
        }
    } else {
        std::sort(prusa_profiles.begin(), prusa_profiles.end(), [](std::reference_wrapper<const std::string> a, std::reference_wrapper<const std::string> b) {
            return a.get() < b.get();
            });
        std::sort(other_profiles.begin(), other_profiles.end(), [](std::reference_wrapper<const std::string> a, std::reference_wrapper<const std::string> b) {
            return a.get() < b.get();
            });
    }
    
    list->Clear();
    if (add_All_item)
        list->append(_L("(All)"), &EMPTY);
    for (const auto& item : prusa_profiles)
        list->append(item, &const_cast<std::string&>(item.get()));
    for (const auto& item : other_profiles)
        list->append(item, &const_cast<std::string&>(item.get()));
}     

void PageMaterials::sort_list_data(PresetList* list, const std::vector<ProfilePrintData>& data)
{
    // sort data
    // then prusa profiles
    // then the rest
    // in alphabetical order
    std::vector<ProfilePrintData> prusa_profiles;
    std::vector<ProfilePrintData> other_profiles;
    //for (int i = 0; i < data.size(); ++i) {
    for (const auto& item : data) {
        const std::string& name = item.name;
        if (name.find("Prusa") != std::string::npos)
            prusa_profiles.emplace_back(item);
        else
            other_profiles.emplace_back(item);
    }
    std::sort(prusa_profiles.begin(), prusa_profiles.end(), [](ProfilePrintData a, ProfilePrintData b) {
        return a.name.get() < b.name.get();
        });
    std::sort(other_profiles.begin(), other_profiles.end(), [](ProfilePrintData a, ProfilePrintData b) {
        return a.name.get() < b.name.get();
        });
    list->Clear();
    //for (const auto& item : prusa_profiles)
    for (int i = 0; i < prusa_profiles.size(); ++i) {
        list->append(std::string(prusa_profiles[i].name) + (prusa_profiles[i].omnipresent ? "" : " *"), &const_cast<std::string&>(prusa_profiles[i].name.get()));
        list->Check(i, prusa_profiles[i].checked);
    }
    //for (const auto& item : other_profiles)
    for (int i = 0; i < other_profiles.size(); ++i) {
        list->append(std::string(other_profiles[i].name) + (other_profiles[i].omnipresent ? "" : " *"), &const_cast<std::string&>(other_profiles[i].name.get()));
        list->Check(i + prusa_profiles.size(), other_profiles[i].checked);
    }
}

void PageMaterials::select_material(int i)
{
    const bool checked = list_profile->IsChecked(i);

    const std::string& alias_key = list_profile->get_data(i);
    wizard_p()->update_presets_in_config(materials->appconfig_section(), alias_key, checked);
}

void PageMaterials::select_all(bool select)
{
    wxWindowUpdateLocker freeze_guard(this);
    (void)freeze_guard;

    for (unsigned i = 0; i < list_profile->GetCount(); i++) {
        const bool current = list_profile->IsChecked(i);
        if (current != select) {
            list_profile->Check(i, select);
            select_material(i);
        }
    }
}

void PageMaterials::clear()
{
	list_printer->Clear();
    list_type->Clear();
    list_vendor->Clear();
    list_profile->Clear();
	sel_printer_count_prev = wxNOT_FOUND;
    sel_printer_item_prev = wxNOT_FOUND;
    sel_type_prev = wxNOT_FOUND;
    sel_vendor_prev = wxNOT_FOUND;
    presets_loaded = false;
}

void PageMaterials::on_activate()
{
    if (! presets_loaded) {
        wizard_p()->update_materials(materials->technology);
        reload_presets();
    }
    first_paint = true;
}


const char *PageCustom::default_profile_name = "My Settings";

PageCustom::PageCustom(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Custom Printer Setup"), _L("Custom Printer"))
{
    cb_custom = new wxCheckBox(this, wxID_ANY, _L("Define a custom printer profile"));
    tc_profile_name = new wxTextCtrl(this, wxID_ANY, default_profile_name);
    auto *label = new wxStaticText(this, wxID_ANY, _L("Custom profile name:"));

    tc_profile_name->Enable(false);
    tc_profile_name->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
        if (tc_profile_name->GetValue().IsEmpty()) {
            if (profile_name_prev.IsEmpty()) { tc_profile_name->SetValue(default_profile_name); }
            else { tc_profile_name->SetValue(profile_name_prev); }
        } else {
            profile_name_prev = tc_profile_name->GetValue();
        }
        evt.Skip();
    });

    cb_custom->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
        tc_profile_name->Enable(custom_wanted());
        wizard_p()->on_custom_setup(custom_wanted());
		
    });

    append(cb_custom);
    append(label);
    append(tc_profile_name);
}

PageUpdate::PageUpdate(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Automatic updates"), _L("Updates"))
    , version_check(true)
    , preset_update(true)
{
    const AppConfig *app_config = wxGetApp().app_config;
    auto boldfont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    boldfont.SetWeight(wxFONTWEIGHT_BOLD);

    auto *box_slic3r = new wxCheckBox(this, wxID_ANY, _L("Check for application updates"));
    box_slic3r->SetValue(false);// app_config->get("version_check") == "1"); // default to no, becasue the conf is never updated anyway. Remove that change if corrected later.
    append(box_slic3r);
    append_text(wxString::Format(_L(
        "If enabled, %s checks for new application versions online. When a new version becomes available, "
         "a notification is displayed at the next application startup (never during program usage). "
         "This is only a notification mechanisms, no automatic installation is done."), SLIC3R_APP_NAME));

    append_spacer(VERTICAL_SPACING);

    auto *box_presets = new wxCheckBox(this, wxID_ANY, _L("Update built-in Presets automatically"));
    box_presets->SetValue(false);// app_config->get("preset_update") == "1"); // default to no, becasue the conf is never updated anyway. Remove that change if corrected later.
    append(box_presets);
    append_text(wxString::Format(_L(
        "If enabled, %s downloads updates of built-in system presets in the background."
        "These updates are downloaded into a separate temporary location."
        "When a new preset version becomes available it is offered at application startup."), SLIC3R_APP_NAME));
    const auto text_bold = _L("Updates are never applied without user's consent and never overwrite user's customized settings.");
    auto *label_bold = new wxStaticText(this, wxID_ANY, text_bold);
    label_bold->SetFont(boldfont);
    label_bold->Wrap(WRAP_WIDTH);
    append(label_bold);
    append_text(_L("Additionally a backup snapshot of the whole configuration is created before an update is applied."));

    box_slic3r->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) { this->version_check = event.IsChecked(); });
    box_presets->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) { this->preset_update = event.IsChecked(); });
}

PageReloadFromDisk::PageReloadFromDisk(ConfigWizard* parent)
    : ConfigWizardPage(parent, _L("Reload from disk"), _L("Reload from disk"))
    , full_pathnames(false)
{
    auto* box_pathnames = new wxCheckBox(this, wxID_ANY, _L("Export full pathnames of models and parts sources into 3mf and amf files"));
    box_pathnames->SetValue(wxGetApp().app_config->get("export_sources_full_pathnames") == "1");
    append(box_pathnames);
    append_text(_L(
        "If enabled, allows the Reload from disk command to automatically find and load the files when invoked.\n"
        "If not enabled, the Reload from disk command will ask to select each file using an open file dialog."
    ));

    box_pathnames->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) { this->full_pathnames = event.IsChecked(); });
}

#if ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
#ifdef _WIN32
PageFilesAssociation::PageFilesAssociation(ConfigWizard* parent)
    : ConfigWizardPage(parent, _L("Files association"), _L("Files association"))
{
    cb_3mf = new wxCheckBox(this, wxID_ANY, _L("Associate .3mf files to " SLIC3R_APP_NAME));
    cb_stl = new wxCheckBox(this, wxID_ANY, _L("Associate .stl files to " SLIC3R_APP_NAME));
//    cb_gcode = new wxCheckBox(this, wxID_ANY, _L("Associate .gcode files to PrusaSlicer G-code Viewer"));

    append(cb_3mf);
    append(cb_stl);
//    append(cb_gcode);
}
#endif // _WIN32
#endif // ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN

PageMode::PageMode(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("View mode"), _L("View mode"))
{
    append_text(_L(SLIC3R_APP_NAME "'s user interfaces comes in three variants:\nSimple, Advanced, and Expert.\n"
        "The Simple mode shows only the most frequently used settings relevant for regular 3D printing. "
        "The other two offer progressively more sophisticated fine-tuning, "
        "they are suitable for advanced and expert users, respectively."));

    radio_simple = new wxRadioButton(this, wxID_ANY, _L("Simple mode"));
    radio_advanced = new wxRadioButton(this, wxID_ANY, _L("Advanced mode"));
    radio_expert = new wxRadioButton(this, wxID_ANY, _L("Expert mode"));

    append(radio_simple);
    append(radio_advanced);
    append(radio_expert);

    append_text("\n" + _L("The size of the object can be specified in inches"));
    check_inch = new wxCheckBox(this, wxID_ANY, _L("Use inches"));
    append(check_inch);
}

void PageMode::on_activate()
{
    std::string mode { "simple" };
    wxGetApp().app_config->get("", "view_mode", mode);

    if (mode == "advanced") { radio_advanced->SetValue(true); }
    else if (mode == "expert") { radio_expert->SetValue(true); }
    else { radio_simple->SetValue(true); }

    check_inch->SetValue(wxGetApp().app_config->get("use_inches") == "1");
}

void PageMode::serialize_mode(AppConfig *app_config) const
{
    std::string mode = "";

    if (radio_simple->GetValue()) { mode = "simple"; }
    if (radio_advanced->GetValue()) { mode = "advanced"; }
    if (radio_expert->GetValue()) { mode = "expert"; }

    // If "Mode" page wasn't selected (no one radiobutton is checked),
    // we shouldn't to update a view_mode value in app_config
    if (mode.empty())
        return; 

    app_config->set("view_mode", mode);
    app_config->set("use_inches", check_inch->GetValue() ? "1" : "0");
}

#ifdef ALLOW_PRUSA_FIRST
PageVendors::PageVendors(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Other Vendors"), _L("Other Vendors"))
{
    const AppConfig &appconfig = this->wizard_p()->appconfig_new;

    append_text(wxString::Format(_L("Pick another vendor supported by %s"), SLIC3R_APP_NAME) + ":");

    auto boldfont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    boldfont.SetWeight(wxFONTWEIGHT_BOLD);

    for (const auto &pair : wizard_p()->bundles) {
        const VendorProfile *vendor = pair.second.vendor_profile;
        if (vendor->id == PresetBundle::PRUSA_BUNDLE) { continue; }

        auto *cbox = new wxCheckBox(this, wxID_ANY, vendor->name);
        cbox->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent &event) {
            wizard_p()->on_3rdparty_install(vendor, cbox->IsChecked());
        });

        const auto &vendors = appconfig.vendors();
        const bool enabled = vendors.find(pair.first) != vendors.end();
        if (enabled) {
            cbox->SetValue(true);

            auto pages = wizard_p()->pages_3rdparty.find(vendor->id);
            wxCHECK_RET(pages != wizard_p()->pages_3rdparty.end(), "Internal error: 3rd party vendor printers page not created");

            for (PagePrinters* page : { pages->second.first, pages->second.second })
                if (page) page->install = true;
        }

        append(cbox);
    }
}
#endif

PageFirmware::PageFirmware(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Firmware Type"), _L("Firmware"), 1)
    , gcode_opt(*print_config_def.get("gcode_flavor"))
    , gcode_picker(nullptr)
{
    append_text(_L("Choose the type of firmware used by your printer."));
    append_text(_(gcode_opt.tooltip));

    wxArrayString choices;
    choices.Alloc(gcode_opt.enum_labels.size());
    for (const auto &label : gcode_opt.enum_labels) {
        choices.Add(label);
    }

    gcode_picker = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    const auto &enum_values = gcode_opt.enum_values;
    auto needle = enum_values.cend();
    if (gcode_opt.default_value) {
        needle = std::find(enum_values.cbegin(), enum_values.cend(), gcode_opt.default_value->serialize());
    }
    if (needle != enum_values.cend()) {
        gcode_picker->SetSelection(needle - enum_values.cbegin());
    } else {
        gcode_picker->SetSelection(0);
    }

    append(gcode_picker);
}

void PageFirmware::apply_custom_config(DynamicPrintConfig &config)
{
    auto sel = gcode_picker->GetSelection();
    if (sel >= 0 && (size_t)sel < gcode_opt.enum_labels.size()) {
        auto *opt = new ConfigOptionEnum<GCodeFlavor>(static_cast<GCodeFlavor>(sel));
        config.set_key_value("gcode_flavor", opt);
    }
}

PageBedShape::PageBedShape(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Bed Shape and Size"), _L("Bed Shape"), 1)
    , shape_panel(new BedShapePanel(this))
{
    append_text(_L("Set the shape of your printer's bed."));

    shape_panel->build_panel(*wizard_p()->custom_config->option<ConfigOptionPoints>("bed_shape"),
        *wizard_p()->custom_config->option<ConfigOptionString>("bed_custom_texture"),
        *wizard_p()->custom_config->option<ConfigOptionString>("bed_custom_model"));

    append(shape_panel);
}

void PageBedShape::apply_custom_config(DynamicPrintConfig &config)
{
    const std::vector<Vec2d>& points = shape_panel->get_shape();
    const std::string& custom_texture = shape_panel->get_custom_texture();
    const std::string& custom_model = shape_panel->get_custom_model();
    config.set_key_value("bed_shape", new ConfigOptionPoints(points));
    config.set_key_value("bed_custom_texture", new ConfigOptionString(custom_texture));
    config.set_key_value("bed_custom_model", new ConfigOptionString(custom_model));
}

PageDiameters::PageDiameters(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Filament and Nozzle Diameters"), _L("Print Diameters"), 1)
    , spin_nozzle(new wxSpinCtrlDouble(this, wxID_ANY))
    , spin_filam(new wxSpinCtrlDouble(this, wxID_ANY))
{
    spin_nozzle->SetDigits(2);
    spin_nozzle->SetIncrement(0.1);
    auto *default_nozzle = print_config_def.get("nozzle_diameter")->get_default_value<ConfigOptionFloats>();
    spin_nozzle->SetValue(default_nozzle != nullptr && default_nozzle->size() > 0 ? default_nozzle->get_at(0) : 0.5);

    spin_filam->SetDigits(2);
    spin_filam->SetIncrement(0.25);
    auto *default_filam = print_config_def.get("filament_diameter")->get_default_value<ConfigOptionFloats>();
    spin_filam->SetValue(default_filam != nullptr && default_filam->size() > 0 ? default_filam->get_at(0) : 3.0);

    append_text(_L("Enter the diameter of your printer's hot end nozzle."));

    auto *sizer_nozzle = new wxFlexGridSizer(3, 5, 5);
    auto *text_nozzle = new wxStaticText(this, wxID_ANY, _L("Nozzle Diameter:"));
    auto *unit_nozzle = new wxStaticText(this, wxID_ANY, _L("mm"));
    sizer_nozzle->AddGrowableCol(0, 1);
    sizer_nozzle->Add(text_nozzle, 0, wxALIGN_CENTRE_VERTICAL);
    sizer_nozzle->Add(spin_nozzle);
    sizer_nozzle->Add(unit_nozzle, 0, wxALIGN_CENTRE_VERTICAL);
    append(sizer_nozzle);

    append_spacer(VERTICAL_SPACING);

    append_text(_L("Enter the diameter of your filament."));
    append_text(_L("Good precision is required, so use a caliper and do multiple measurements along the filament, then compute the average."));

    auto *sizer_filam = new wxFlexGridSizer(3, 5, 5);
    auto *text_filam = new wxStaticText(this, wxID_ANY, _L("Filament Diameter:"));
    auto *unit_filam = new wxStaticText(this, wxID_ANY, _L("mm"));
    sizer_filam->AddGrowableCol(0, 1);
    sizer_filam->Add(text_filam, 0, wxALIGN_CENTRE_VERTICAL);
    sizer_filam->Add(spin_filam);
    sizer_filam->Add(unit_filam, 0, wxALIGN_CENTRE_VERTICAL);
    append(sizer_filam);
}

void PageDiameters::apply_custom_config(DynamicPrintConfig &config)
{
    auto *opt_nozzle = new ConfigOptionFloats(1, spin_nozzle->GetValue());
    config.set_key_value("nozzle_diameter", opt_nozzle);
    auto *opt_filam = new ConfigOptionFloats(1, spin_filam->GetValue());
    config.set_key_value("filament_diameter", opt_filam);

    config.set_key_value("support_material_extrusion_width", new ConfigOptionFloatOrPercent(95, false));
    config.set_key_value("top_infill_extrusion_width", new ConfigOptionFloatOrPercent(100, false));
    config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(140, false));
    config.set_key_value("extrusion_width", new ConfigOptionFloatOrPercent(110, false));
    config.set_key_value("perimeter_extrusion_width", new ConfigOptionFloatOrPercent(110, false));
    config.set_key_value("external_perimeter_extrusion_width", new ConfigOptionFloatOrPercent(105, false));
    config.set_key_value("infill_extrusion_width", new ConfigOptionFloatOrPercent(110, false));
    config.set_key_value("solid_infill_extrusion_width", new ConfigOptionFloatOrPercent(110, false));
    config.set_key_value("solid_infill_extrusion_width", new ConfigOptionFloatOrPercent(110, false));

}

PageTemperatures::PageTemperatures(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Nozzle and Bed Temperatures"), _L("Temperatures"), 1)
    , spin_extr(new wxSpinCtrlDouble(this, wxID_ANY))
    , spin_bed(new wxSpinCtrlDouble(this, wxID_ANY))
{
    spin_extr->SetIncrement(5.0);
    const auto &def_extr = *print_config_def.get("temperature");
    spin_extr->SetRange(def_extr.min, def_extr.max);
    auto *default_extr = def_extr.get_default_value<ConfigOptionInts>();
    spin_extr->SetValue(default_extr != nullptr && default_extr->size() > 0 ? default_extr->get_at(0) : 200);

    spin_bed->SetIncrement(5.0);
    const auto &def_bed = *print_config_def.get("bed_temperature");
    spin_bed->SetRange(def_bed.min, def_bed.max);
    auto *default_bed = def_bed.get_default_value<ConfigOptionInts>();
    spin_bed->SetValue(default_bed != nullptr && default_bed->size() > 0 ? default_bed->get_at(0) : 0);

    append_text(_L("Enter the temperature needed for extruding your filament."));
    append_text(_L("A rule of thumb is 160 to 230 °C for PLA, and 215 to 250 °C for ABS."));

    auto *sizer_extr = new wxFlexGridSizer(3, 5, 5);
    auto *text_extr = new wxStaticText(this, wxID_ANY, _L("Extrusion Temperature:"));
    auto *unit_extr = new wxStaticText(this, wxID_ANY, _L("°C"));
    sizer_extr->AddGrowableCol(0, 1);
    sizer_extr->Add(text_extr, 0, wxALIGN_CENTRE_VERTICAL);
    sizer_extr->Add(spin_extr);
    sizer_extr->Add(unit_extr, 0, wxALIGN_CENTRE_VERTICAL);
    append(sizer_extr);

    append_spacer(VERTICAL_SPACING);

    append_text(_L("Enter the bed temperature needed for getting your filament to stick to your heated bed."));
    append_text(_L("A rule of thumb is 60 °C for PLA and 110 °C for ABS. Leave zero if you have no heated bed."));

    auto *sizer_bed = new wxFlexGridSizer(3, 5, 5);
    auto *text_bed = new wxStaticText(this, wxID_ANY, _L("Bed Temperature:"));
    auto *unit_bed = new wxStaticText(this, wxID_ANY, _L("°C"));
    sizer_bed->AddGrowableCol(0, 1);
    sizer_bed->Add(text_bed, 0, wxALIGN_CENTRE_VERTICAL);
    sizer_bed->Add(spin_bed);
    sizer_bed->Add(unit_bed, 0, wxALIGN_CENTRE_VERTICAL);
    append(sizer_bed);
}

void PageTemperatures::apply_custom_config(DynamicPrintConfig &config)
{
    auto *opt_extr = new ConfigOptionInts(1, spin_extr->GetValue());
    config.set_key_value("temperature", opt_extr);
    auto *opt_extr1st = new ConfigOptionInts(1, spin_extr->GetValue());
    config.set_key_value("first_layer_temperature", opt_extr1st);
    auto *opt_bed = new ConfigOptionInts(1, spin_bed->GetValue());
    config.set_key_value("bed_temperature", opt_bed);
    auto *opt_bed1st = new ConfigOptionInts(1, spin_bed->GetValue());
    config.set_key_value("first_layer_bed_temperature", opt_bed1st);
}


// Index

ConfigWizardIndex::ConfigWizardIndex(wxWindow *parent)
    : wxPanel(parent)
    , bg(ScalableBitmap(parent, "Slic3r_192px_transparent.png", 192))
    , bullet_black(ScalableBitmap(parent, "bullet_black.png"))
    , bullet_blue(ScalableBitmap(parent, "bullet_blue.png"))
    , bullet_white(ScalableBitmap(parent, "bullet_white.png"))
    , item_active(NO_ITEM)
    , item_hover(NO_ITEM)
    , last_page((size_t)-1)
{
    SetMinSize(bg.bmp().GetSize());

    const wxSize size = GetTextExtent("m");
    em_w = size.x;
    em_h = size.y;

    // Add logo bitmap.
    // This could be done in on_paint() along with the index labels, but I've found it tricky
    // to get the bitmap rendered well on all platforms with transparent background.
    // In some cases it didn't work at all. And so wxStaticBitmap is used here instead,
    // because it has all the platform quirks figured out.
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    logo = new wxStaticBitmap(this, wxID_ANY, bg.bmp());
    sizer->AddStretchSpacer();
    sizer->Add(logo);
    SetSizer(sizer);

    Bind(wxEVT_PAINT, &ConfigWizardIndex::on_paint, this);
    Bind(wxEVT_MOTION, &ConfigWizardIndex::on_mouse_move, this);

    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &evt) {
        if (item_hover != -1) {
            item_hover = -1;
            Refresh();
        }
        evt.Skip();
    });

    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &evt) {
        if (item_hover >= 0) { go_to(item_hover); }
    });
}

wxDECLARE_EVENT(EVT_INDEX_PAGE, wxCommandEvent);

void ConfigWizardIndex::add_page(ConfigWizardPage *page)
{
    last_page = items.size();
    items.emplace_back(Item { page->shortname, page->indent, page });
    Refresh();
}

void ConfigWizardIndex::add_label(wxString label, unsigned indent)
{
    items.emplace_back(Item { std::move(label), indent, nullptr });
    Refresh();
}

ConfigWizardPage* ConfigWizardIndex::active_page() const
{
    if (item_active >= items.size()) { return nullptr; }

    return items[item_active].page;
}

void ConfigWizardIndex::go_prev()
{
    // Search for a preceiding item that is a page (not a label, ie. page != nullptr)

    if (item_active == NO_ITEM) { return; }

    for (size_t i = item_active; i > 0; i--) {
        if (items[i - 1].page != nullptr) {
            go_to(i - 1);
            return;
        }
    }
}

void ConfigWizardIndex::go_next()
{
    // Search for a next item that is a page (not a label, ie. page != nullptr)

    if (item_active == NO_ITEM) { return; }

    for (size_t i = item_active + 1; i < items.size(); i++) {
        if (items[i].page != nullptr) {
            go_to(i);
            return;
        }
    }
}

// This one actually performs the go-to op
void ConfigWizardIndex::go_to(size_t i)
{
    if (i != item_active
        && i < items.size()
        && items[i].page != nullptr) {
        auto *new_active = items[i].page;
        auto *former_active = active_page();
        if (former_active != nullptr) {
            former_active->Hide();
        }

        item_active = i;
        new_active->Show();

        wxCommandEvent evt(EVT_INDEX_PAGE, GetId());
        AddPendingEvent(evt);

        Refresh();

        new_active->on_activate();
    }
}

void ConfigWizardIndex::go_to(const ConfigWizardPage *page)
{
    if (page == nullptr) { return; }

    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].page == page) {
            go_to(i);
            return;
        }
    }
}

void ConfigWizardIndex::clear()
{
    auto *former_active = active_page();
    if (former_active != nullptr) { former_active->Hide(); }

    items.clear();
    item_active = NO_ITEM;
}

void ConfigWizardIndex::on_paint(wxPaintEvent & evt)
{
    const auto size = GetClientSize();
    if (size.GetHeight() == 0 || size.GetWidth() == 0) { return; }

    wxPaintDC dc(this);

    const auto bullet_w = bullet_black.bmp().GetSize().GetWidth();
    const auto bullet_h = bullet_black.bmp().GetSize().GetHeight();
    const int yoff_icon = bullet_h < em_h ? (em_h - bullet_h) / 2 : 0;
    const int yoff_text = bullet_h > em_h ? (bullet_h - em_h) / 2 : 0;
    const int yinc = item_height();

    int index_width = 0;

    unsigned y = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        unsigned x = em_w/2 + item.indent * em_w;

        if (i == item_active || (item_hover >= 0 && i == (size_t)item_hover)) {
            dc.DrawBitmap(bullet_blue.bmp(), x, y + yoff_icon, false);
        }
        else if (i < item_active)  { dc.DrawBitmap(bullet_black.bmp(), x, y + yoff_icon, false); }
        else if (i > item_active)  { dc.DrawBitmap(bullet_white.bmp(), x, y + yoff_icon, false); }

        x += + bullet_w + em_w/2;
        const auto text_size = dc.GetTextExtent(item.label);
        dc.DrawText(item.label, x, y + yoff_text);

        y += yinc;
        index_width = std::max(index_width, (int)x + text_size.x);
    }

    if (GetMinSize().x < index_width) {
        CallAfter([this, index_width]() {
            SetMinSize(wxSize(index_width, GetMinSize().y));
            Refresh();
        });
    }
}

void ConfigWizardIndex::on_mouse_move(wxMouseEvent &evt)
{
    const wxClientDC dc(this);
    const wxPoint pos = evt.GetLogicalPosition(dc);

    const ssize_t item_hover_new = pos.y / item_height();

	if (item_hover_new < ssize_t(items.size()) && item_hover_new != item_hover) {
        item_hover = item_hover_new;
        Refresh();
    }

    evt.Skip();
}

void ConfigWizardIndex::msw_rescale()
{
    const wxSize size = GetTextExtent("m");
    em_w = size.x;
    em_h = size.y;

    bg.msw_rescale();
    SetMinSize(bg.bmp().GetSize());
    logo->SetBitmap(bg.bmp());

    bullet_black.msw_rescale();
    bullet_blue.msw_rescale();
    bullet_white.msw_rescale();
    Refresh();
}


// Materials

const std::string Materials::UNKNOWN = "(Unknown)";

void Materials::push(const Preset *preset)
{
    presets.emplace_back(preset);
    types.insert(technology & T_FFF
        ? Materials::get_filament_type(preset)
        : Materials::get_material_type(preset));
}

void  Materials::add_printer(const Preset* preset)
{
	printers.insert(preset);
}

void Materials::clear()
{
    presets.clear();
    types.clear();
	printers.clear();
    compatibility_counter.clear();
}

const std::string& Materials::appconfig_section() const
{
    return (technology & T_FFF) ? AppConfig::SECTION_FILAMENTS : AppConfig::SECTION_MATERIALS;
}

const std::string& Materials::get_type(const Preset *preset) const
{
    return (technology & T_FFF) ? get_filament_type(preset) : get_material_type(preset);
}

const std::string& Materials::get_vendor(const Preset *preset) const
{
    return (technology & T_FFF) ? get_filament_vendor(preset) : get_material_vendor(preset);
}

const std::string& Materials::get_filament_type(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionStrings>("filament_type");
    if (opt != nullptr && opt->values.size() > 0) {
        return opt->values[0];
    } else {
        return UNKNOWN;
    }
}

const std::string& Materials::get_filament_vendor(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionString>("filament_vendor");
    return opt != nullptr ? opt->value : UNKNOWN;
}

const std::string& Materials::get_material_type(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionString>("material_type");
    if (opt != nullptr) {
        return opt->value;
    } else {
        return UNKNOWN;
    }
}

const std::string& Materials::get_material_vendor(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionString>("material_vendor");
    return opt != nullptr ? opt->value : UNKNOWN;
}

// priv

void ConfigWizard::priv::load_pages()
{
    wxWindowUpdateLocker freeze_guard(q);
    (void)freeze_guard;

    const ConfigWizardPage *former_active = index->active_page();

    index->clear();

    index->add_page(page_welcome);
#ifdef ALLOW_PRUSA_FIRST
    // Printers
    index->add_page(page_fff);
    index->add_page(page_msla);
    index->add_page(page_vendors);
    for (const auto &pages : pages_3rdparty) {
        for (PagePrinters* page : { pages.second.first, pages.second.second })
            if (page && page->install)
                index->add_page(page);
    }
#else
    for (PagePrinters *page : pages_vendors) {
        index->add_page(page);
    }
#endif

    index->add_page(page_custom);
    if (page_custom->custom_wanted()) {
        index->add_page(page_firmware);
        index->add_page(page_bed);
        index->add_page(page_diams);
        index->add_page(page_temps);
    }

    // Filaments & Materials
    if (any_fff_selected) { index->add_page(page_filaments); }
    if (any_sla_selected) { index->add_page(page_sla_materials); }

    // there should to be selected at least one printer
    btn_finish->Enable(any_fff_selected || any_sla_selected || custom_printer_selected);

    index->add_page(page_update);
    index->add_page(page_reload_from_disk);
#if ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
#ifdef _WIN32
    index->add_page(page_files_association);
#endif // _WIN32
#endif // ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
    index->add_page(page_mode);

    index->go_to(former_active);   // Will restore the active item/page if possible

    q->Layout();
}

void ConfigWizard::priv::init_dialog_size()
{
    // Clamp the Wizard size based on screen dimensions

    const auto idx = wxDisplay::GetFromWindow(q);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);

    const auto disp_rect = display.GetClientArea();
    wxRect window_rect(
        disp_rect.x + disp_rect.width / 20,
        disp_rect.y + disp_rect.height / 20,
        9*disp_rect.width / 10,
        9*disp_rect.height / 10);

    const int width_hint = index->GetSize().GetWidth() + 900+/*page_fff->get_width()*/ + 30 * em();    // XXX: magic constant, I found no better solution
    if (width_hint < window_rect.width) {
        window_rect.x += (window_rect.width - width_hint) / 2;
        window_rect.width = width_hint;
    }

    q->SetSize(window_rect);
}

void ConfigWizard::priv::load_vendors()
{
    bundles = BundleMap::load();

    // Load up the set of vendors / models / variants the user has had enabled up till now
    AppConfig *app_config = wxGetApp().app_config;
    appconfig_new.set_vendors(*app_config);

    // Initialize the is_visible flag in printer Presets
    for (auto &pair : bundles) {
        pair.second.preset_bundle->load_installed_printers(appconfig_new);
    }

    // Copy installed filaments and SLA material names from app_config to appconfig_new
    // while resolving current names of profiles, which were renamed in the meantime.
    for (PrinterTechnology technology : { ptFFF, ptSLA }) {
    	const std::string &section_name = (technology == ptFFF) ? AppConfig::SECTION_FILAMENTS : AppConfig::SECTION_MATERIALS;
		std::map<std::string, std::string> section_new;
		if (app_config->has_section(section_name)) {
			const std::map<std::string, std::string> &section_old = app_config->get_section(section_name);
			for (const std::pair<std::string, std::string> &material_name_and_installed : section_old)
				if (material_name_and_installed.second == "1") {
					// Material is installed. Resolve it in bundles.
                    size_t num_found = 0;
					const std::string &material_name = material_name_and_installed.first;
				    for (auto &bundle : bundles) {
				    	const PresetCollection &materials = bundle.second.preset_bundle->materials(technology);
				    	const Preset           *preset    = materials.find_preset(material_name);
				    	if (preset == nullptr) {
				    		// Not found. Maybe the material preset is there, bu it was was renamed?
							const std::string *new_name = materials.get_preset_name_renamed(material_name);
							if (new_name != nullptr)
								preset = materials.find_preset(*new_name);
				    	}
                        if (preset != nullptr) {
                            // Materal preset was found, mark it as installed.
                            section_new[preset->name] = "1";
                            ++ num_found;
                        }
				    }
                    if (num_found == 0)
            	        BOOST_LOG_TRIVIAL(error) << boost::format("Profile %1% was not found in installed vendor Preset Bundles.") % material_name;
                    else if (num_found > 1)
            	        BOOST_LOG_TRIVIAL(error) << boost::format("Profile %1% was found in %2% vendor Preset Bundles.") % material_name % num_found;
                }
		}
        appconfig_new.set_section(section_name, section_new);
    };
}

void ConfigWizard::priv::add_page(ConfigWizardPage *page)
{
    const int proportion = (page->shortname == _L("Filaments")) || (page->shortname == _L("SLA Materials")) ? 1 : 0;
    hscroll_sizer->Add(page, proportion, wxEXPAND);
    all_pages.push_back(page);
}

void ConfigWizard::priv::enable_next(bool enable)
{
    btn_next->Enable(enable);
    btn_finish->Enable(enable);
}

void ConfigWizard::priv::set_start_page(ConfigWizard::StartPage start_page)
{
    switch (start_page) {
        case ConfigWizard::SP_PRINTERS:
#ifdef ALLOW_PRUSA_FIRST
            index->go_to(page_fff); 
            btn_next->SetFocus();
#else
            if(pages_vendors.empty())
            {
                index->go_to(page_welcome);
                btn_next->SetFocus();
            }
            else
            {
                index->go_to(pages_vendors.front());
                btn_next->SetFocus();
            }
#endif
            break;
        case ConfigWizard::SP_FILAMENTS:
            index->go_to(page_filaments);
            btn_finish->SetFocus();
            break;
        case ConfigWizard::SP_MATERIALS:
            index->go_to(page_sla_materials);
            btn_finish->SetFocus();
            break;
        default:
            index->go_to(page_welcome);
            btn_next->SetFocus();
            break;
    }
}

#ifdef ALLOW_PRUSA_FIRST
void ConfigWizard::priv::create_3rdparty_pages()
{
    for (const auto &pair : bundles) {
        const VendorProfile *vendor = pair.second.vendor_profile;
        if (vendor->id == PresetBundle::PRUSA_BUNDLE) { continue; }

        bool is_fff_technology = false;
        bool is_sla_technology = false;

        for (auto& model: vendor->models)
        {
            if (!is_fff_technology && model.technology == ptFFF)
                 is_fff_technology = true;
            if (!is_sla_technology && model.technology == ptSLA)
                 is_sla_technology = true;
        }

        PagePrinters* pageFFF = nullptr;
        PagePrinters* pageSLA = nullptr;

        if (is_fff_technology) {
            pageFFF = new PagePrinters(q, vendor->name + " " +_L("FFF Technology Printers"), vendor->name+" FFF", *vendor, 1, T_FFF);
            add_page(pageFFF);
        }

        if (is_sla_technology) {
            pageSLA = new PagePrinters(q, vendor->name + " " + _L("SLA Technology Printers"), vendor->name+" MSLA", *vendor, 1, T_SLA);
            add_page(pageSLA);
        }

        pages_3rdparty.insert({vendor->id, {pageFFF, pageSLA}});
    }
}
#endif

void ConfigWizard::priv::set_run_reason(RunReason run_reason)
{
    this->run_reason = run_reason;
    for (auto &page : all_pages) {
        page->set_run_reason(run_reason);
    }
}

void ConfigWizard::priv::update_materials(Technology technology)
{
    if (any_fff_selected && (technology & T_FFF)) {
        filaments.clear();
        aliases_fff.clear();
        // Iterate filaments in all bundles
        for (const auto &pair : bundles) {
            for (const auto &filament : pair.second.preset_bundle->filaments) {
                // Check if filament is already added
                if (filaments.containts(&filament))
					continue;
                // Iterate printers in all bundles
                for (const auto &printer : pair.second.preset_bundle->printers) {
					if (!printer.is_visible || printer.printer_technology() != ptFFF)
						continue;
                    // Filter out inapplicable printers
					if (is_compatible_with_printer(PresetWithVendorProfile(filament, filament.vendor), PresetWithVendorProfile(printer, printer.vendor))) {
						if (!filaments.containts(&filament)) {
							filaments.push(&filament);
							if (!filament.alias.empty())
								aliases_fff[filament.alias].insert(filament.name); 
						} 
						filaments.add_printer(&printer);
                    }
				}
				
            }
        }
        // count compatible printers
        for (const auto& preset : filaments.presets) {

            const auto filter = [preset](const std::pair<std::string, size_t> element) {
                return preset->alias == element.first;
            };
            if (std::find_if(filaments.compatibility_counter.begin(), filaments.compatibility_counter.end(), filter) != filaments.compatibility_counter.end()) {
                continue;
            }
            std::vector<size_t> idx_with_same_alias;
            for (size_t i = 0; i < filaments.presets.size(); ++i) {
                if (preset->alias == filaments.presets[i]->alias)
                    idx_with_same_alias.push_back(i);
            }
            size_t counter = 0;
            for (const auto& printer : filaments.printers) {
                if (!(*printer).is_visible || (*printer).printer_technology() != ptFFF)
                    continue;
                bool compatible = false;
                // Test otrher materials with same alias
                for (size_t i = 0; i < idx_with_same_alias.size() && !compatible; ++i) {
                    const Preset& prst = *(filaments.presets[idx_with_same_alias[i]]);
                    const Preset& prntr = *printer;
                    if (is_compatible_with_printer(PresetWithVendorProfile(prst, prst.vendor), PresetWithVendorProfile(prntr, prntr.vendor))) {
                        compatible = true;
                        break;
                    }
                }
                if (compatible)
                    counter++;
            }
            filaments.compatibility_counter.emplace_back(preset->alias, counter);
        }
    }

    if (any_sla_selected && (technology & T_SLA)) {
        sla_materials.clear();
        aliases_sla.clear();

        // Iterate SLA materials in all bundles
        for (const auto &pair : bundles) {
            for (const auto &material : pair.second.preset_bundle->sla_materials) {
                // Check if material is already added
                if (sla_materials.containts(&material))
                	continue;
                // Iterate printers in all bundles
				// For now, we only allow the profiles to be compatible with another profiles inside the same bundle.
                for (const auto& printer : pair.second.preset_bundle->printers) {
                    if(!printer.is_visible || printer.printer_technology() != ptSLA)
                        continue;
                    // Filter out inapplicable printers
                    if (is_compatible_with_printer(PresetWithVendorProfile(material, nullptr), PresetWithVendorProfile(printer, nullptr))) {
                        // Check if material is already added
                        if(!sla_materials.containts(&material)) {
                            sla_materials.push(&material);
                            if (!material.alias.empty())
                                aliases_sla[material.alias].insert(material.name);
                        }
                        sla_materials.add_printer(&printer);
                    }
                }
            }
        }
        // count compatible printers        
        for (const auto& preset : sla_materials.presets) {
            
            const auto filter = [preset](const std::pair<std::string, size_t> element) {
                return preset->alias == element.first;
            };
            if (std::find_if(sla_materials.compatibility_counter.begin(), sla_materials.compatibility_counter.end(), filter) != sla_materials.compatibility_counter.end()) {
                continue;
            }
            std::vector<size_t> idx_with_same_alias;
            for (size_t i = 0; i < sla_materials.presets.size(); ++i) {
                if(preset->alias == sla_materials.presets[i]->alias)
                    idx_with_same_alias.push_back(i);
            }
            size_t counter = 0;
            for (const auto& printer : sla_materials.printers) {
                if (!(*printer).is_visible || (*printer).printer_technology() != ptSLA)
                    continue;
                bool compatible = false;
                // Test otrher materials with same alias
                for (size_t i = 0; i < idx_with_same_alias.size() && !compatible; ++i) {
                    const Preset& prst = *(sla_materials.presets[idx_with_same_alias[i]]);
                    const Preset& prntr = *printer;
                    if (is_compatible_with_printer(PresetWithVendorProfile(prst, prst.vendor), PresetWithVendorProfile(prntr, prntr.vendor))) {
                        compatible = true;
                        break;
                    }
                }
                if (compatible)
                    counter++;
            }
            sla_materials.compatibility_counter.emplace_back(preset->alias, counter);
        }
    }
}

void ConfigWizard::priv::on_custom_setup(const bool custom_wanted)
{
	custom_printer_selected = custom_wanted;
    load_pages();
}

void ConfigWizard::priv::on_printer_pick(PagePrinters *page, const PrinterPickerEvent &evt)
{
    if (check_sla_selected() != any_sla_selected ||
        check_fff_selected() != any_fff_selected) {
        any_fff_selected = check_fff_selected();
        any_sla_selected = check_sla_selected();

        load_pages();
    }

    // Update the is_visible flag on relevant printer profiles
    for (auto &pair : bundles) {
        if (pair.first != evt.vendor_id) { continue; }

        for (auto &preset : pair.second.preset_bundle->printers) {
            if (preset.config.opt_string("printer_model") == evt.model_id
                && preset.config.opt_string("printer_variant") == evt.variant_name) {
                preset.is_visible = evt.enable;
            }
        }

        // When a printer model is picked, but there is no material installed compatible with this printer model,
        // install default materials for selected printer model silently.
		check_and_install_missing_materials(page->technology, evt.model_id);
    }

    if (page->technology & T_FFF) {
        page_filaments->clear();
    } else if (page->technology & T_SLA) {
        page_sla_materials->clear();
    }
}

void ConfigWizard::priv::select_default_materials_for_printer_model(const VendorProfile::PrinterModel &printer_model, Technology technology)
{
    PageMaterials* page_materials = technology & T_FFF ? page_filaments : page_sla_materials;
    for (const std::string& material : printer_model.default_materials)
        appconfig_new.set(page_materials->materials->appconfig_section(), material, "1");
}

void ConfigWizard::priv::select_default_materials_for_printer_models(Technology technology, const std::set<const VendorProfile::PrinterModel*> &printer_models)
{
    PageMaterials     *page_materials    = technology & T_FFF ? page_filaments : page_sla_materials;
    const std::string &appconfig_section = page_materials->materials->appconfig_section();

    auto select_default_materials_for_printer_page = [this, appconfig_section, printer_models](PagePrinters *page_printers, Technology technology)
    {
        const std::string vendor_id = page_printers->get_vendor_id();
        for (auto& pair : bundles)
            if (pair.first == vendor_id)
            	for (const VendorProfile::PrinterModel *printer_model : printer_models)
    		        for (const std::string &material : printer_model->default_materials)
			            appconfig_new.set(appconfig_section, material, "1");
    };

#ifdef ALLOW_PRUSA_FIRST
    PagePrinters* page_printers = technology & T_FFF ? page_fff : page_msla;
    select_default_materials_for_printer_page(page_printers, technology);

    for (const auto& printer : pages_3rdparty) 
    {
        page_printers = technology & T_FFF ? printer.second.first : printer.second.second;
        if (page_printers)
            select_default_materials_for_printer_page(page_printers, technology);
    }
#else
    for (PagePrinters* page_printers : pages_vendors)
    {
        if (technology == page_printers->technology) {
            select_default_materials_for_printer_page(page_printers, technology);
        }
    }

#endif

    update_materials(technology);
    ((technology & T_FFF) ? page_filaments : page_sla_materials)->reload_presets();
}

#ifdef ALLOW_PRUSA_FIRST
void ConfigWizard::priv::on_3rdparty_install(const VendorProfile *vendor, bool install)
{
    auto it = pages_3rdparty.find(vendor->id);
    wxCHECK_RET(it != pages_3rdparty.end(), "Internal error: GUI page not found for 3rd party vendor profile");

    for (PagePrinters* page : { it->second.first, it->second.second }) 
        if (page) {
            if (page->install && !install)
                page->select_all(false);
            page->install = install;
            // if some 3rd vendor is selected, select first printer for them
            if (install)
                page->printer_pickers[0]->select_one(0, true);
            page->Layout();
        }

    load_pages();
}
#endif

bool ConfigWizard::priv::on_bnt_finish()
{
    /* When Filaments or Sla Materials pages are activated, 
     * materials for this pages are automaticaly updated and presets are reloaded.
     * 
     * But, if _Finish_ button was clicked without activation of those pages 
     * (for example, just some printers were added/deleted), 
     * than last changes wouldn't be updated for filaments/materials.
     * SO, do that before close of Wizard
     */
    update_materials(T_ANY);
    if (any_fff_selected)
        page_filaments->reload_presets();
    if (any_sla_selected)
        page_sla_materials->reload_presets();

	// theres no need to check that filament is selected if we have only custom printer
    if (custom_printer_selected && !any_fff_selected && !any_sla_selected) return true;
    // check, that there is selected at least one filament/material
    return check_and_install_missing_materials(T_ANY);
}

// This allmighty method verifies, whether there is at least a single compatible filament or SLA material installed
// for each Printer preset of each Printer Model installed.
//
// In case only_for_model_id is set, then the test is done for that particular printer model only, and the default materials are installed silently.
// Otherwise the user is quieried whether to install the missing default materials or not.
// 
// Return true if the tested Printer Models already had materials installed.
// Return false if there were some Printer Models with missing materials, independent from whether the defaults were installed for these
// respective Printer Models or not.
bool ConfigWizard::priv::check_and_install_missing_materials(Technology technology, const std::string &only_for_model_id)
{
	// Walk over all installed Printer presets and verify whether there is a filament or SLA material profile installed at the same PresetBundle,
	// which is compatible with it.
    const auto printer_models_missing_materials = [this, only_for_model_id](PrinterTechnology technology, const std::string &section)
    {
		const std::map<std::string, std::string> &appconfig_presets = appconfig_new.has_section(section) ? appconfig_new.get_section(section) : std::map<std::string, std::string>();
    	std::set<const VendorProfile::PrinterModel*> printer_models_without_material;
        for (const auto &pair : bundles) {
        	const PresetCollection &materials = pair.second.preset_bundle->materials(technology);
        	for (const auto &printer : pair.second.preset_bundle->printers) {
                if (printer.is_visible && printer.printer_technology() == technology) {
	            	const VendorProfile::PrinterModel *printer_model = PresetUtils::system_printer_model(printer);
	            	assert(printer_model != nullptr);
	            	if ((only_for_model_id.empty() || only_for_model_id == printer_model->id) &&
	            		printer_models_without_material.find(printer_model) == printer_models_without_material.end()) {
                    	bool has_material = false;
			            for (const std::pair<std::string, std::string> &preset : appconfig_presets) {
			            	if (preset.second == "1") {
			            		const Preset *material = materials.find_preset(preset.first, false);
			            		if (material != nullptr && is_compatible_with_printer(PresetWithVendorProfile(*material, nullptr), PresetWithVendorProfile(printer, nullptr))) {
				                	has_material = true;
				                    break;
				                }
			                }
			            }
			            if (! has_material)
			            	printer_models_without_material.insert(printer_model);
			        }
                }
            }
        }
        assert(printer_models_without_material.empty() || only_for_model_id.empty() || only_for_model_id == (*printer_models_without_material.begin())->id);
        return printer_models_without_material;
    };

    const auto ask_and_select_default_materials = [this](const wxString &message, const std::set<const VendorProfile::PrinterModel*> &printer_models, Technology technology)
    {
        wxMessageDialog msg(q, message, _L("Notice"), wxYES_NO);
        if (msg.ShowModal() == wxID_YES)
            select_default_materials_for_printer_models(technology, printer_models);
    };

    const auto printer_model_list = [](const std::set<const VendorProfile::PrinterModel*> &printer_models) -> wxString {
    	wxString out;
    	for (const VendorProfile::PrinterModel *printer_model : printer_models) {
            wxString name = from_u8(printer_model->name);
    		out += "\t\t";
    		out += name;
    		out += "\n";
    	}
    	return out;
    };

    if (any_fff_selected && (technology & T_FFF)) {
    	std::set<const VendorProfile::PrinterModel*> printer_models_without_material = printer_models_missing_materials(ptFFF, AppConfig::SECTION_FILAMENTS);
    	if (! printer_models_without_material.empty()) {
			if (only_for_model_id.empty())
				ask_and_select_default_materials(
					_L("The following FFF printer models have no filament selected:") +
					"\n\n\t" +
					printer_model_list(printer_models_without_material) +
					"\n\n\t" +
					_L("Do you want to select default filaments for these FFF printer models?"),
					printer_models_without_material,
					T_FFF);
			else
				select_default_materials_for_printer_model(**printer_models_without_material.begin(), T_FFF);
			return false;
		}
    }

    if (any_sla_selected && (technology & T_SLA)) {
    	std::set<const VendorProfile::PrinterModel*> printer_models_without_material = printer_models_missing_materials(ptSLA, AppConfig::SECTION_MATERIALS);
    	if (! printer_models_without_material.empty()) {
	        if (only_for_model_id.empty())
	            ask_and_select_default_materials(
					_L("The following SLA printer models have no materials selected:") +
	            	"\n\n\t" +
				   	printer_model_list(printer_models_without_material) +
					"\n\n\t" +
					_L("Do you want to select default SLA materials for these printer models?"),
					printer_models_without_material,
	            	T_SLA);
	        else
				select_default_materials_for_printer_model(**printer_models_without_material.begin(), T_SLA);
	        return false;
	    }
    }

    return true;
}

void ConfigWizard::priv::apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdater *updater)
{
    const auto enabled_vendors = appconfig_new.vendors();

    // Install bundles from resources if needed:
    std::vector<std::string> install_bundles;
    for (const auto &pair : bundles) {
        if (! pair.second.is_in_resources) { continue; }

        if (pair.second.is_prusa_bundle) {
            // Always install Prusa bundle, because it has a lot of filaments/materials
            // likely to be referenced by other profiles.
            install_bundles.emplace_back(pair.first);
            continue;
        }

        const auto vendor = enabled_vendors.find(pair.first);
        if (vendor == enabled_vendors.end()) { continue; }

        size_t size_sum = 0;
        for (const auto &model : vendor->second) { size_sum += model.second.size(); }

        if (size_sum > 0) {
            // This vendor needs to be installed
            install_bundles.emplace_back(pair.first);
        }
    }

    // Decide whether to create snapshot based on run_reason and the reset profile checkbox
    bool snapshot = true;
    Snapshot::Reason snapshot_reason = Snapshot::SNAPSHOT_UPGRADE;
    switch (run_reason) {
        case ConfigWizard::RR_DATA_EMPTY:
            snapshot = false;
            break;
        case ConfigWizard::RR_DATA_LEGACY:
            snapshot = true;
            break;
        case ConfigWizard::RR_DATA_INCOMPAT:
            // In this case snapshot has already been taken by
            // PresetUpdater with the appropriate reason
            snapshot = false;
            break;
        case ConfigWizard::RR_USER:
            snapshot = page_welcome->reset_user_profile();
            snapshot_reason = Snapshot::SNAPSHOT_USER;
            break;
    }

    if (snapshot) {
        SnapshotDB::singleton().take_snapshot(*app_config, snapshot_reason);
    }

    if (install_bundles.size() > 0) {
        // Install bundles from resources.
        // Don't create snapshot - we've already done that above if applicable.
        updater->install_bundles_rsrc(std::move(install_bundles), false);
    } else {
        BOOST_LOG_TRIVIAL(info) << "No bundles need to be installed from resources";
    }

    if (page_welcome->reset_user_profile()) {
        BOOST_LOG_TRIVIAL(info) << "Resetting user profiles...";
        preset_bundle->reset(true);
    }

    app_config->set_vendors(appconfig_new);
    if (appconfig_new.has_section(AppConfig::SECTION_FILAMENTS)) {
        app_config->set_section(AppConfig::SECTION_FILAMENTS, appconfig_new.get_section(AppConfig::SECTION_FILAMENTS));
    }
    if (appconfig_new.has_section(AppConfig::SECTION_MATERIALS)) {
        app_config->set_section(AppConfig::SECTION_MATERIALS, appconfig_new.get_section(AppConfig::SECTION_MATERIALS));
    }
    app_config->set("version_check", page_update->version_check ? "1" : "0");
    app_config->set("preset_update", page_update->preset_update ? "1" : "0");
    app_config->set("export_sources_full_pathnames", page_reload_from_disk->full_pathnames ? "1" : "0");

#if ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
#ifdef _WIN32
    app_config->set("associate_3mf", page_files_association->associate_3mf() ? "1" : "0");
    app_config->set("associate_stl", page_files_association->associate_stl() ? "1" : "0");
//    app_config->set("associate_gcode", page_files_association->associate_gcode() ? "1" : "0");

    if (wxGetApp().is_editor()) {
        if (page_files_association->associate_3mf())
            wxGetApp().associate_3mf_files();
        if (page_files_association->associate_stl())
            wxGetApp().associate_stl_files();
    }
//    else {
//        if (page_files_association->associate_gcode())
//            wxGetApp().associate_gcode_files();
//    }

#endif // _WIN32
#endif // ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN

    page_mode->serialize_mode(app_config);

    std::string preferred_model;

    // Figure out the default pre-selected printer based on the selections in the pickers.
    // The default is the first selected printer model (one with at least 1 variant selected).
    // The default is only applied by load_presets() if the user doesn't have a (visible) printer
    // selected already.
    // Prusa printers are considered first, then 3rd party.
    const auto config_prusa = enabled_vendors.find("PrusaResearch");
    if (config_prusa != enabled_vendors.end()) {
        for (const auto &model : bundles.prusa_bundle().vendor_profile->models) {
            const auto model_it = config_prusa->second.find(model.id);
            if (model_it != config_prusa->second.end() && model_it->second.size() > 0) {
                preferred_model = model.id;
                break;
            }
        }
    }
    if (preferred_model.empty()) {
        for (const auto &bundle : bundles) {
            if (bundle.second.is_prusa_bundle) { continue; }

            const auto config = enabled_vendors.find(bundle.first);
            if (config == enabled_vendors.end()) { continue; }
            for (const auto &model : bundle.second.vendor_profile->models) {
                const auto model_it = config->second.find(model.id);
                if (model_it != config->second.end() && model_it->second.size() > 0) {
                    preferred_model = model.id;
                    break;
                }
            }
        }
    }

    preset_bundle->load_presets(*app_config, preferred_model);

    if (page_custom->custom_wanted()) {
        page_firmware->apply_custom_config(*custom_config);
        page_bed->apply_custom_config(*custom_config);
        page_diams->apply_custom_config(*custom_config);
        page_temps->apply_custom_config(*custom_config);

        const std::string profile_name = page_custom->profile_name();
        preset_bundle->load_config_from_wizard(profile_name, *custom_config);
    }

    // Update the selections from the compatibilty.
    preset_bundle->export_selections(*app_config);
}


static const boost::unordered_map<PrinterTechnology, std::string> tech_to_string{ {
    { PrinterTechnology::ptFFF, "FFF" },
    { PrinterTechnology::ptSLA, "SLA" },
    { PrinterTechnology::ptSLS, "SLS" },
    } };

void ConfigWizard::priv::update_presets_in_config(const std::string& section, const std::string& alias_key, bool add)
{
    const PresetAliases& aliases = section == AppConfig::SECTION_FILAMENTS ? aliases_fff : aliases_sla;

    auto update = [this, add](const std::string& s, const std::string& key) {
    	assert(! s.empty());
        if (add)
            appconfig_new.set(s, key, "1");
        else
            appconfig_new.erase(s, key); 
    };

    // add or delete presets had a same alias 
    auto it = aliases.find(alias_key);
    if (it != aliases.end())
        for (const std::string& name : it->second)
            update(section, name);
}

bool ConfigWizard::priv::check_fff_selected()
{
#ifdef ALLOW_PRUSA_FIRST
    bool ret = page_fff->any_selected();
    for (const auto& printer: pages_3rdparty)
        if (printer.second.first)               // FFF page
            ret |= printer.second.first->any_selected();
#else
    bool ret = false;
    for (const PagePrinters *printer : pages_vendors)
        if (printer->technology == T_FFF)
            ret |= printer->any_selected();
#endif
    return ret;
}

bool ConfigWizard::priv::check_sla_selected()
{
#ifdef ALLOW_PRUSA_FIRST
    bool ret = page_msla->any_selected();
    for (const auto& printer: pages_3rdparty)
        if (printer.second.second)               // SLA page
            ret |= printer.second.second->any_selected();
#else
    bool ret = false;
    for (const PagePrinters *printer : pages_vendors)
        if (printer->technology == T_SLA)
            ret |= printer->any_selected();
#endif
    return ret;
}


// Public

ConfigWizard::ConfigWizard(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _(name()), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , p(new priv(this))
{
    this->SetFont(wxGetApp().normal_font());

    p->load_vendors();
    p->custom_config.reset(DynamicPrintConfig::new_from_defaults_keys({
        "gcode_flavor", "bed_shape", "bed_custom_texture", "bed_custom_model", "nozzle_diameter", "filament_diameter", "temperature", "bed_temperature",
    }));

    p->index = new ConfigWizardIndex(this);

    auto *vsizer = new wxBoxSizer(wxVERTICAL);
    auto *topsizer = new wxBoxSizer(wxHORIZONTAL);
    auto *hline = new wxStaticLine(this);
    p->btnsizer = new wxBoxSizer(wxHORIZONTAL);

    // Initially we _do not_ SetScrollRate in order to figure out the overall width of the Wizard  without scrolling.
    // Later, we compare that to the size of the current screen and set minimum width based on that (see below).
    p->hscroll = new wxScrolledWindow(this);
    p->hscroll_sizer = new wxBoxSizer(wxHORIZONTAL);
    p->hscroll->SetSizer(p->hscroll_sizer);

    topsizer->Add(p->index, 0, wxEXPAND);
    topsizer->AddSpacer(INDEX_MARGIN);
    topsizer->Add(p->hscroll, 1, wxEXPAND);

    p->btn_sel_all = new wxButton(this, wxID_ANY, _L("Select all standard printers in this page"));
    p->btnsizer->Add(p->btn_sel_all);

    p->btn_prev = new wxButton(this, wxID_ANY, _L("< &Back"));
    p->btn_next = new wxButton(this, wxID_ANY, _L("&Next >"));
    p->btn_finish = new wxButton(this, wxID_APPLY, _L("&Finish"));
    p->btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));   // Note: The label needs to be present, otherwise we get accelerator bugs on Mac
    p->btnsizer->AddStretchSpacer();
    p->btnsizer->Add(p->btn_prev, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_next, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_finish, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_cancel, 0, wxLEFT, BTN_SPACING);

    const auto prusa_it = p->bundles.find("PrusaResearch");
    wxCHECK_RET(prusa_it != p->bundles.cend(), "Vendor PrusaResearch not found");
    const VendorProfile *vendor_prusa = prusa_it->second.vendor_profile;

    p->add_page(p->page_welcome = new PageWelcome(this));

#ifdef ALLOW_PRUSA_FIRST
    p->page_fff = new PagePrinters(this, _L("Prusa FFF Technology Printers"), "Prusa FFF", *vendor_prusa, 0, T_FFF);
    p->add_page(p->page_fff);

    p->page_msla = new PagePrinters(this, _L("Prusa MSLA Technology Printers"), "Prusa MSLA", *vendor_prusa, 0, T_SLA);
    p->add_page(p->page_msla);

    // Pages for 3rd party vendors
    p->create_3rdparty_pages();   // Needs to be done _before_ creating PageVendors
    p->add_page(p->page_vendors = new PageVendors(this));
    p->add_page(p->page_custom = new PageCustom(this));
    p->custom_printer_selected = p->page_custom->custom_wanted();
#else
    std::vector<std::string> sorted_vendors;
    for (const auto &vendor : p->bundles) sorted_vendors.push_back(vendor.first);
    std::sort(sorted_vendors.begin(), sorted_vendors.end());

    for (const std::string &vendor_name : sorted_vendors) {
        const Bundle &vendor = p->bundles[vendor_name];
        bool first = true;
        for (const PrinterTechnology &tech : vendor.vendor_profile->technologies) {
            wxString name = _(L(vendor.vendor_profile->name));
            name.Replace("{technology}", tech_to_string.at(tech));
            wxString description = _(L(vendor.vendor_profile->full_name));
            description.Replace("{technology}", tech_to_string.at(tech));
            p->pages_vendors.push_back(new PagePrinters(this, description, name, *vendor.vendor_profile, (uint32_t)(vendor.vendor_profile->technologies.size()>1 && !first ? 1 : 0), (Technology)(uint8_t)tech));
            p->add_page(p->pages_vendors.back());
            first = false;
        }
    }
#endif

    p->any_sla_selected = p->check_sla_selected();
    p->any_fff_selected = p->check_fff_selected();

    p->update_materials(T_ANY);

    p->add_page(p->page_filaments = new PageMaterials(this, &p->filaments,
        _L("Filament Profiles Selection"), _L("Filaments"), _L("Type:") ));
    p->add_page(p->page_sla_materials = new PageMaterials(this, &p->sla_materials,
        _L("SLA Material Profiles Selection") + " ", _L("SLA Materials"), _L("Type:") ));

    p->add_page(p->page_custom   = new PageCustom(this));
    
    p->add_page(p->page_update   = new PageUpdate(this));
    p->add_page(p->page_reload_from_disk = new PageReloadFromDisk(this));
#if ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
#ifdef _WIN32
    p->add_page(p->page_files_association = new PageFilesAssociation(this));
#endif // _WIN32
#endif // ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
    p->add_page(p->page_mode     = new PageMode(this));
    p->add_page(p->page_firmware = new PageFirmware(this));
    p->add_page(p->page_bed      = new PageBedShape(this));
    p->add_page(p->page_diams    = new PageDiameters(this));
    p->add_page(p->page_temps    = new PageTemperatures(this));

    p->load_pages();
    p->index->go_to(size_t{0});

    vsizer->Add(topsizer, 1, wxEXPAND | wxALL, DIALOG_MARGIN);
    vsizer->Add(hline, 0, wxEXPAND);
    vsizer->Add(p->btnsizer, 0, wxEXPAND | wxALL, DIALOG_MARGIN);

    SetSizer(vsizer);
    SetSizerAndFit(vsizer);

    // We can now enable scrolling on hscroll
    p->hscroll->SetScrollRate(30, 30);

    on_window_geometry(this, [this]() {
        p->init_dialog_size();
    });

    p->btn_prev->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &) { this->p->index->go_prev(); });

    p->btn_next->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &)
    {
        // check, that there is selected at least one filament/material
        ConfigWizardPage* active_page = this->p->index->active_page();
        if (// Leaving the filaments or SLA materials page and 
        	(active_page == p->page_filaments || active_page == p->page_sla_materials) && 
        	// some Printer models had no filament or SLA material selected.
        	! p->check_and_install_missing_materials(dynamic_cast<PageMaterials*>(active_page)->materials->technology))
        	// In that case don't leave the page and the function above queried the user whether to install default materials.
            return;
        this->p->index->go_next();
    });

    p->btn_finish->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &)
    {
        if (p->on_bnt_finish())
            this->EndModal(wxID_OK);
    });

    p->btn_sel_all->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &) {
#ifdef ALLOW_PRUSA_FIRST
        p->any_sla_selected = true;
        p->load_pages();
        p->page_fff->select_all(true, false);
        p->page_msla->select_all(true, false);
        p->index->go_to(p->page_mode);
#else
        ConfigWizardPage *page = p->index->active_page();
        PagePrinters *page_printers = dynamic_cast<PagePrinters*>(page);
        if (page_printers)
            page_printers->select_all(true, false);
        p->index->go_to(p->page_update);
#endif
    });

    p->index->Bind(EVT_INDEX_PAGE, [this](const wxCommandEvent &) {
        const bool is_last = p->index->active_is_last();
        p->btn_next->Show(! is_last);
        if (is_last)
            p->btn_finish->SetFocus();

        Layout();
    });

    if (wxLinux_gtk3)
        this->Bind(wxEVT_SHOW, [this, vsizer](const wxShowEvent& e) {
            ConfigWizardPage* active_page = p->index->active_page();
            if (!active_page)
                return;
            for (auto page : p->all_pages)
                if (page != active_page)
                    page->Hide();
            // update best size for the dialog after hiding of the non-active pages
            vsizer->SetSizeHints(this);
            // set initial dialog size
            p->init_dialog_size();
        });
}

ConfigWizard::~ConfigWizard() {}

bool ConfigWizard::run(RunReason reason, StartPage start_page)
{
    BOOST_LOG_TRIVIAL(info) << boost::format("Running ConfigWizard, reason: %1%, start_page: %2%") % reason % start_page;

    GUI_App &app = wxGetApp();

    p->set_run_reason(reason);
    p->set_start_page(start_page);

    if (ShowModal() == wxID_OK) {
        p->apply_config(app.app_config, app.preset_bundle, app.preset_updater);
        app.app_config->set_legacy_datadir(false);
        app.update_mode();
        app.obj_manipul()->update_ui_from_settings();
        BOOST_LOG_TRIVIAL(info) << "ConfigWizard applied";
        return true;
    } else {
        BOOST_LOG_TRIVIAL(info) << "ConfigWizard cancelled";
        return false;
    }
}

const wxString& ConfigWizard::name(const bool from_menu/* = false*/)
{
    // A different naming convention is used for the Wizard on Windows & GTK vs. OSX.
    // Note: Don't call _() macro here.
    //       This function just return the current name according to the OS.
    //       Translation is implemented inside GUI_App::add_config_menu()
#if __APPLE__
    static const wxString config_wizard_name =  L("Configuration Assistant");
    static const wxString config_wizard_name_menu = L("Configuration &Assistant");
#else
    static const wxString config_wizard_name = L("Configuration Wizard");
    static const wxString config_wizard_name_menu = L("Configuration &Wizard");
#endif
    return from_menu ? config_wizard_name_menu : config_wizard_name;
}

void ConfigWizard::on_dpi_changed(const wxRect &suggested_rect)
{
    p->index->msw_rescale();

    const int em = em_unit();

    msw_buttons_rescale(this, em, { wxID_APPLY, 
                                    wxID_CANCEL,
                                    p->btn_sel_all->GetId(),
                                    p->btn_next->GetId(),
                                    p->btn_prev->GetId() });

#ifdef ALLOW_PRUSA_FIRST
    for (auto printer_picker : p->page_fff->printer_pickers)
        msw_buttons_rescale(this, em, printer_picker->get_button_indexes());
#else
    for (PagePrinters* page : p->pages_vendors)
        for (PrinterPicker* printer_picker : page->printer_pickers)
            msw_buttons_rescale(this, em, printer_picker->get_button_indexes());
#endif

    p->init_dialog_size();

    Refresh();
}

}
}
