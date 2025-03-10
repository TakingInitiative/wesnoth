/*
   Copyright (C) 2003 - 2008 by David White <dave@whitevine.net>
                 2008 - 2015 by Ignacio Riquelme Morelle <shadowm2006@gmail.com>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "addon/manager_ui.hpp"

#include "addon/info.hpp"
#include "addon/manager.hpp"
#include "addon/state.hpp"
#include "filesystem.hpp"
#include "formula/string_utils.hpp"
#include "game_preferences.hpp"
#include "gettext.hpp"
#include "gui/dialogs/addon/manager.hpp"
#include "gui/dialogs/addon/description.hpp"
#include "gui/dialogs/addon/filter_options.hpp"
#include "gui/dialogs/addon/uninstall_list.hpp"
#include "gui/dialogs/addon/connect.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/transient_message.hpp"
#include "gui/widgets/window.hpp"
#include "gui/widgets/settings.hpp"
#include "help/help_button.hpp"
#include "image.hpp"
#include "log.hpp"
#include "font/marked-up_text.hpp"
#include "font/standard_colors.hpp"
#include "wml_separators.hpp"
#include "wml_exception.hpp"

#include "addon/client.hpp"

#include <boost/dynamic_bitset.hpp>

static lg::log_domain log_config("config");
static lg::log_domain log_network("network");
static lg::log_domain log_filesystem("filesystem");
static lg::log_domain log_addons_client("addons-client");

#define ERR_CFG LOG_STREAM(err,   log_config)

#define ERR_NET LOG_STREAM(err,   log_network)

#define ERR_FS  LOG_STREAM(err,   log_filesystem)

#define ERR_AC  LOG_STREAM(err ,  log_addons_client)
#define WRN_AC  LOG_STREAM(warn,  log_addons_client)
#define LOG_AC  LOG_STREAM(info,  log_addons_client)
#define DBG_AC  LOG_STREAM(debug, log_addons_client)

namespace {

inline const addon_info& addon_at(const std::string& id, const addons_list& addons)
{
	addons_list::const_iterator it = addons.find(id);
	assert(it != addons.end());
	return it->second;
}

bool get_addons_list(addons_client& client, addons_list& list)
{
	list.clear();

	config cfg;
	client.request_addons_list(cfg);

	if(!cfg) {
		return false;
	}

	read_addons_list(cfg, list);

	return true;
}

const std::string color_upgradable = font::color2markup(font::YELLOW_COLOR);
const std::string color_outdated = "<255,127,0>";

std::string describe_addon_status(const addon_tracking_info& info)
{
	switch(info.state) {
	case ADDON_NONE:
		return info.can_publish ? _("addon_state^Published, not installed") : _("addon_state^Not installed");
	case ADDON_INSTALLED:
	case ADDON_NOT_TRACKED:
		// Consider add-ons without version information as installed
		// for the main display. Their Description info should elaborate
		// on their status.
		return font::GOOD_TEXT + (
			info.can_publish ? _("addon_state^Published") : _("addon_state^Installed"));
	case ADDON_INSTALLED_UPGRADABLE:
		return color_upgradable + (
			info.can_publish ? _("addon_state^Published, upgradable") : _("addon_state^Installed, upgradable"));
	case ADDON_INSTALLED_OUTDATED:
		return color_outdated + (
			info.can_publish ? _("addon_state^Published, outdated on server") : _("addon_state^Installed, outdated on server"));
	case ADDON_INSTALLED_BROKEN:
		return font::BAD_TEXT + (
			info.can_publish ? _("addon_state^Published, broken") : _("addon_state^Installed, broken"));
	default:
		return font::color2markup(font::GRAY_COLOR) + _("addon_state^Unknown");
	}
}

/** Performs all backend and UI actions for taking down the specified add-on. */
void do_remote_addon_delete(CVideo& video, addons_client& client, const std::string& addon_id)
{
	utils::string_map symbols;
	symbols["addon"] = make_addon_title(addon_id); // FIXME: need the real title!
	const std::string& text = vgettext("Deleting '$addon|' will permanently erase its download and upload counts on the add-ons server. Do you really wish to continue?", symbols);

	const int res = gui2::show_message(
		video, _("Confirm"), text, gui2::dialogs::message::yes_no_buttons);

	if(res != gui2::window::OK) {
		return;
	}

	std::string server_msg;
	if(!client.delete_remote_addon(addon_id, server_msg)) {
		gui2::show_error_message(video,
			_("The server responded with an error:") + "\n" +
			client.get_last_server_error());
	} else {
		// FIXME: translation needed!
		gui2::show_transient_message(video, _("Response"), server_msg);
	}
}

/** Performs all backend and UI actions for publishing the specified add-on. */
void do_remote_addon_publish(CVideo& video, addons_client& client, const std::string& addon_id, const version_info& remote_version)
{
	std::string server_msg;

	config cfg = get_addon_pbl_info(addon_id);

	const version_info& version_to_publish = cfg["version"].str();

	if(version_to_publish <= remote_version) {
		const int res = gui2::show_message(video, _("Warning"),
			_("The remote version of this add-on is greater or equal to the version being uploaded. Do you really wish to continue?"),
			gui2::dialogs::message::yes_no_buttons);

		if(res != gui2::window::OK) {
			return;
		}
	}

	if(!image::exists(cfg["icon"].str())) {
		gui2::show_error_message(video, _("Invalid icon path. Make sure the path points to a valid image."));
	} else if(!client.request_distribution_terms(server_msg)) {
		gui2::show_error_message(video,
			_("The server responded with an error:") + "\n" +
			client.get_last_server_error());
	} else if(gui2::show_message(video, _("Terms"), server_msg, gui2::dialogs::message::ok_cancel_buttons) == gui2::window::OK) {
		if(!client.upload_addon(addon_id, server_msg, cfg)) {
			gui2::show_error_message(video,
				_("The server responded with an error:") + "\n" +
				client.get_last_server_error());
		} else {
			gui2::show_transient_message(video, _("Response"), server_msg);
		}
	}
}

/** GUI1 support class handling the button used to display add-on descriptions. */
class description_display_action : public gui::dialog_button_action
{
	CVideo& v_;
	std::vector<std::string> display_ids_;
	addons_list addons_;
	addons_tracking_list tracking_;
	gui::filter_textbox* filter_;

public:
	description_display_action(CVideo& v, const std::vector<std::string>& display_ids, const addons_list& addons, const addons_tracking_list& tracking, gui::filter_textbox* filter)
		: v_(v) , display_ids_(display_ids), addons_(addons), tracking_(tracking), filter_(filter)
	{}

	virtual gui::dialog_button_action::RESULT button_pressed(int filter_choice)
	{
		assert(filter_ != nullptr);

		const int menu_selection = filter_->get_index(filter_choice);
		if(menu_selection < 0) { return gui::CONTINUE_DIALOG; }

		const size_t choice = static_cast<size_t>(menu_selection);
		if(choice < display_ids_.size()) {
			const std::string& id = display_ids_[choice];
			assert(tracking_.find(id) != tracking_.end());
			gui2::dialogs::addon_description::display(id, addons_, tracking_, v_);
		}

		return gui::CONTINUE_DIALOG;
	}
};

/** Struct type for storing filter options. */
struct addons_filter_state
{
	std::string keywords;
	boost::dynamic_bitset<> types;
	ADDON_STATUS_FILTER status;
	// Yes, the sorting criterion and direction are part of the
	// filter options since changing them requires rebuilding the
	// dialog list contents.
	ADDON_SORT sort;
	ADDON_SORT_DIRECTION direction;
	bool changed;

	addons_filter_state()
		: keywords()
		, types(ADDON_TYPES_COUNT)
		, status(FILTER_ALL)
		, sort(SORT_NAMES)
		, direction(DIRECTION_ASCENDING)
		, changed(false)
	{
		types.flip();
	}
};

/** GUI1 support class handling the filter options button. */
class filter_options_action : public gui::dialog_button_action
{
	CVideo& video_;
	addons_filter_state& f_;

public:
	filter_options_action(CVideo& video, addons_filter_state& filter)
		: video_(video)
		, f_(filter)
	{}

	virtual gui::dialog_button_action::RESULT button_pressed(int)
	{
		gui2::dialogs::addon_filter_options dlg;

		dlg.set_displayed_status(f_.status);
		dlg.set_displayed_types(f_.types);
		dlg.set_sort(f_.sort);
		dlg.set_direction(f_.direction);

		dlg.show(video_);

		const boost::dynamic_bitset<> new_types = dlg.displayed_types();
		const ADDON_STATUS_FILTER new_status = dlg.displayed_status();
		const ADDON_SORT new_sort = dlg.sort();
		const ADDON_SORT_DIRECTION new_direction = dlg.direction();

		assert(f_.types.size() == new_types.size());

		if(f_.types == new_types && f_.status == new_status &&
		   f_.sort == new_sort && f_.direction == new_direction) {
			// Close the manager dialog only if the filter options changed.
			return gui::CONTINUE_DIALOG;
		}

		f_.types = new_types;
		f_.status = new_status;
		f_.sort = new_sort;
		f_.direction = new_direction;
		f_.changed = true;

		return gui::CLOSE_DIALOG;
	}
};

/**
 * Comparator type used for sorting the add-ons list according to the user's preferences.
 */
struct addon_pointer_list_sorter
{
	addon_pointer_list_sorter(ADDON_SORT sort, ADDON_SORT_DIRECTION direction)
		: sort_(sort), dir_(direction)
	{}

	inline bool operator()(const addons_list::value_type* a, const addons_list::value_type* b) {
		assert(a != nullptr && b != nullptr);

		if(dir_ == DIRECTION_DESCENDING) {
			const addons_list::value_type* c = a;
			a = b;
			b = c;
		}

		switch(sort_) {
		case SORT_NAMES:
			// Alphanumerical by name, case insensitive.
			return utf8::lowercase(a->second.title) < utf8::lowercase(b->second.title);
		case SORT_UPDATED:
			// Numerical by last upload TS.
			return a->second.updated < b->second.updated;
		case SORT_CREATED:
		default:
			// Numerical by first upload TS (or the equivalent campaignd WML order).
			return a->second.order < b->second.order;
		}
	}

private:
	ADDON_SORT sort_;
	ADDON_SORT_DIRECTION dir_;
};

/** Shorthand type for the sorted add-ons list. */
typedef std::vector<const addons_list::value_type*> sorted_addon_pointer_list;

/**
 * Sorts the user-visible add-ons list according to the user's preferences.
 *
 * The internal add-ons list is actually implemented employing an associative
 * container to map individual list entries to add-on ids for faster look-ups.
 * The visible form of the list may actually include more elements than just
 * the contents of the add-ons server; more specifically, it may include
 * Publish and Delete entries for local add-ons with .pbl files.
 *
 * The GUI1 list/menu class does not support horizontal scrolling, which
 * results in a very limited set of information columns that can be displayed
 * safely without running out of space and causing content to be omitted, and
 * clicking on any column header to change the sort also affects the
 * Publish/Delete entries by necessity. These two factors combined make it
 * inconvenient at this time to just use the GUI1 widget's interface to make
 * it default to a specific sorting criterion.
 *
 * Thus, we need a "neutral" or "fallback" sorting step before feeding the
 * add-ons list's data to the widget and appending Publish/Delete options to
 * it. Since this is definitely not the most evident UI concept in use in this
 * dialog, it is hidden behind the Options dialog and has sensible defaults
 * intended to optimize the add-ons experience; alphanumerical sorting feels
 * natural and breaks any illusion of quality rating or any such that could
 * result from a list default-sorted by first-upload order as done in all
 * versions prior to 1.11.0.
 *
 * This function takes care of sorting the list with minimal memory footprint
 * by passing around a set of pointers to items of the source list in
 * @a addons for use in the dialog building code.
 *
 * @param addons The source add-ons list.
 * @param sort Sorting criterion.
 * @param direction Sorting order (ascending/descending).
 *
 * @return A vector containing pointers to items from @a addons sorted
 *         accordingly. Iterators to items from @a addons <b>must</b> remain
 *         valid for the whole lifespan of this vector.
 */
sorted_addon_pointer_list sort_addons_list(addons_list& addons, ADDON_SORT sort, ADDON_SORT_DIRECTION direction)
{
	sorted_addon_pointer_list res;
	addon_pointer_list_sorter sorter(sort, direction);

	for(const addons_list::value_type& entry : addons) {
		res.push_back(&entry);
	}

	std::stable_sort(res.begin(), res.end(), sorter);

	return res;
}

void show_addons_manager_dialog(CVideo& v, addons_client& client, addons_list& addons, std::string& last_addon_id, bool& stay_in_ui, bool& wml_changed, addons_filter_state& filter)
{
	std::unique_ptr<cursor::setter> cursor_setter(new cursor::setter(cursor::WAIT));

	stay_in_ui = false;
	filter.changed = false;

	const ADDON_STATUS_FILTER prev_view = filter.status;
	assert(prev_view < FILTER_COUNT);

	const bool updates_only =
		filter.status == FILTER_UPGRADABLE;

	const bool show_publish_delete = !updates_only;

	// Currently installed add-ons, which we'll need to check when updating.
	// const std::vector<std::string>& installed_addons_list = installed_addons();

	// Add-ons available for publishing in the remote
	// (i.e. we have .pbl files for them).
	const std::vector<std::string>& can_publish_ids = available_addons();

	// Add-ons available for deleting in the remote
	// (i.e. already published, and we have .pbl files for them).
	std::vector<std::string> can_delete_ids;

	// Status tracking information about add-ons.
	addons_tracking_list tracking;

	// UI markup.
	const std::string sep(1, COLUMN_SEPARATOR);

	// List and list filter control contents.
	std::vector<std::string> options, filter_options;
	std::string header;

	// The add-on ids actually available for the user to pick from in the UI.
	std::vector<std::string> option_ids;

	// UI sorting detail.
	std::vector<int> sort_sizes;

	header = HEADING_PREFIX + sep + _("Name") + sep;
	if(updates_only) {
		header += _("Old Version") + sep + _("New Version") + sep;
	} else {
		header += _("Version") + sep;
	}
	header += _("Author") + sep + _("Size");
	// The Type and Downloads columns aren't displayed when updating because of
	// display space constraints. Presumably, the user doesn't care about that
	// information since the add-on is already installed.
	//
	// Type is also always displayed last so it can get automatically truncated
	// if its translated contents don't fit, instead of truncating other, more
	// important columns such as Size.
	if(!updates_only) {
		header += sep + _("Downloads") + sep + _("Type");
	}
	// end of list header

	options.push_back(header);
	filter_options.push_back(header);

	//
	// Prepare the add-ons list for display and get status
	// information.
	//

	const sorted_addon_pointer_list& sorted_addons = sort_addons_list(addons, filter.sort, filter.direction);

	bool have_upgradable_addons = false;

	for(const sorted_addon_pointer_list::value_type& sorted_entry : sorted_addons) {
		const addons_list::value_type& entry = *sorted_entry;
		const addon_info& addon = entry.second;
		tracking[addon.id] = get_addon_tracking_info(addon);

		const ADDON_STATUS state = tracking[addon.id].state;

		if((filter.status == FILTER_UPGRADABLE && state != ADDON_INSTALLED_UPGRADABLE) ||
		   (filter.status == FILTER_NOT_INSTALLED && state != ADDON_NONE) ||
		   (filter.status == FILTER_INSTALLED && !is_installed_addon_status(state)) ||
		   (!filter.types[addon.type])
		)
			continue;

		if(state == ADDON_INSTALLED_UPGRADABLE) {
			have_upgradable_addons = true;
		}

		option_ids.push_back(addon.id);

		if(tracking[addon.id].can_publish) {
			can_delete_ids.push_back(addon.id);
		}

		const std::string& display_sep = sep;
		const std::string& display_size = size_display_string(addon.size);
		const std::string& display_type = addon.display_type();
		const std::string& display_down = std::to_string(addon.downloads);
		const std::string& display_icon = addon.display_icon();
		const std::string& display_status = describe_addon_status(tracking[addon.id]);

		std::string display_version = addon.version.str();
		std::string display_old_version = tracking[addon.id].installed_version;
		std::string display_title = addon.display_title();
		std::string display_author = addon.author;

		// Add negative sizes to reverse the sort order.
		sort_sizes.push_back(-addon.size);

		std::string row;

		// First we enter information that's used only for filtering.
		// This includes the description, which we cannot display
		// as a normal list row due to space constraints.

		row = display_title + sep;
		if(updates_only) {
			row += display_old_version + sep;
		}
		row += display_version + sep + display_author + sep +
			display_size + sep + display_down + sep +
			display_type + sep + addon.description;

		filter_options.push_back(row);

		// Now we enter information for list row display.
		// Three fields are truncated to accommodate for GUI1's limitations.

		utils::ellipsis_truncate(display_author, 14);

		// Word-wrap the title field to a limit of two lines.
		display_title = font::word_wrap_text(display_title, font::SIZE_NORMAL, 150, -1, 2);

		// Versions are too important in upgrades mode, so don't
		// truncate them then.
		if(!updates_only) {
			utf8::truncate_as_ucs4(display_version, 12);

			if(state == ADDON_INSTALLED_UPGRADABLE || state == ADDON_INSTALLED_OUTDATED) {
				utf8::truncate_as_ucs4(display_old_version, 12);

				if(state == ADDON_INSTALLED_UPGRADABLE) {
					display_version =
						color_upgradable + display_old_version +
						"\n" + color_upgradable + display_version;
				} else {
					display_version =
						color_outdated + display_old_version +
						"\n" + color_outdated + display_version;
				}
			}
		}

		// NOTE: NULL_MARKUP used to escape abuse of formatting chars in add-on titles
		row = IMAGE_PREFIX + display_icon + display_sep + font::NULL_MARKUP + display_title + "\n" + font::color2markup(font::TITLE_COLOR) + font::SMALL_TEXT + display_status + display_sep;
		if(updates_only) {
			row += display_old_version + display_sep;
		}
		row += display_version + display_sep + display_author + display_sep + display_size;
		if(!updates_only) {
			row += display_sep + display_down + display_sep + display_type;
		}

		options.push_back(row);
	}

	if(show_publish_delete) {
		utils::string_map i18n_syms;

		// Enter publish and remote deletion options
		for(const std::string& pub_id : can_publish_ids) {
			i18n_syms["addon_title"] = make_addon_title(pub_id);

			static const std::string publish_icon = "icons/icon-game.png~BLIT(icons/icon-addon-publish.png)";
			const std::string& text = vgettext("Publish: $addon_title", i18n_syms);

			options.push_back(IMAGE_PREFIX + publish_icon + COLUMN_SEPARATOR + font::GOOD_TEXT + text);
			filter_options.push_back(text);
		}
		for(const std::string& del_id : can_delete_ids) {
			i18n_syms["addon_title"] = make_addon_title(del_id);

			static const std::string delete_icon = "icons/icon-game.png~BLIT(icons/icon-addon-delete.png)";
			const std::string& text = vgettext("Delete: $addon_title", i18n_syms);

			options.push_back(IMAGE_PREFIX + delete_icon + COLUMN_SEPARATOR + font::BAD_TEXT + text);
			filter_options.push_back(text);
		}
	}

	// If the options vector is empty it means we don't have publish/delete
	// entries to display, either because there are no add-ons on the server
	// at all, or none match the selected criteria. In such cases, insert a
	// message row informing the player of the situation.

	const bool dummy_addons_list = options.size() == 1; // The header is always there.

	int result;
	// Magic constant assigned to the Update All button as its return value.
	static const int update_all_value = -255;

	/* do */ {
		//
		// Set-up the actual GUI1 dialog and its children.
		//

		std::string dlg_message;

		if(dummy_addons_list) {
			dlg_message = addons.empty()
				? _("There are no add-ons available for download from this server.")
				: _("There are no add-ons matching the specified criteria on this server.");
		}

		gui::dialog dlg(v, _("Add-ons Manager"), dlg_message, gui::OK_CANCEL);

		gui::menu::basic_sorter sorter;
		sorter.set_alpha_sort(1).set_alpha_sort(2).set_alpha_sort(3);
		if(!updates_only) {
			sorter.set_position_sort(4, sort_sizes).set_numeric_sort(5).set_alpha_sort(6);
		} else {
			sorter.set_alpha_sort(4).set_position_sort(5, sort_sizes);
		}

		gui::menu::imgsel_style addon_style(gui::menu::bluebg_style);
		addon_style.scale_images(font::relative_size(72), font::relative_size(72));

		gui::menu* addons_list_menu = new gui::menu(v, options, false, -1,
			gui::dialog::max_menu_width, &sorter, &addon_style, false);
		dlg.set_menu(addons_list_menu);

		std::string filter_label;
		if(!dummy_addons_list) {
			filter_label = _("Filter: ");
		}

		gui::filter_textbox* filter_box = new gui::filter_textbox(v,
			filter_label, options, filter_options, 1, dlg, 300);
		filter_box->set_text(filter.keywords);
		dlg.set_textbox(filter_box);

		description_display_action description_helper(v, option_ids, addons, tracking, filter_box);
		gui::dialog_button* description_button = new gui::dialog_button(v,
			_("Description"), gui::button::TYPE_PRESS, gui::CONTINUE_DIALOG, &description_helper);
		dlg.add_button(description_button, gui::dialog::BUTTON_EXTRA);

		gui::dialog_button* update_all_button = new gui::dialog_button(v, _("Update All"),
			gui::button::TYPE_PRESS, update_all_value);
		update_all_button->enable(have_upgradable_addons);
		dlg.add_button(update_all_button, gui::dialog::BUTTON_EXTRA);

		filter_options_action filter_opts_helper(v, filter);
		gui::dialog_button* filter_opts_button = new gui::dialog_button(v,
			_("filter^Options"), gui::button::TYPE_PRESS, gui::CONTINUE_DIALOG, &filter_opts_helper);
		dlg.add_button(filter_opts_button, gui::dialog::BUTTON_TOP);

		help::help_button* help_button = new help::help_button(v, "installing_addons");
		dlg.add_button(help_button, gui::dialog::BUTTON_HELP);

		// Disable some buttons when there's nothing to display.
		if(dummy_addons_list) {
			filter_box->hide(true);
			description_button->enable(false);
			update_all_button->enable(false);
			addons_list_menu->hide(true);
		}

		// Focus the menu on the previous selection.
		std::vector<std::string>::iterator it = !last_addon_id.empty() ?
			std::find(option_ids.begin(), option_ids.end(), last_addon_id) :
			option_ids.end();

		if(it != option_ids.end()) {
			addons_list_menu->move_selection(std::distance(option_ids.begin(), it));
		}

		cursor_setter.reset();

		//
		// Execute the dialog.
		//
		result = filter_box->get_index(dlg.show());

		filter.keywords = filter_box->text();
	}

	const bool update_everything = result == update_all_value;

	if(result < 0 && !(update_everything || filter.changed)) {
		// User canceled the dialog.
		return;
	}

	stay_in_ui = true;

	if(filter.changed) {
		// The caller will run this function again.
		return;
	}

	if(show_publish_delete) {
		if(result >= int(option_ids.size() + can_publish_ids.size())) {
			// Handle remote deletion.
			const std::string& id = can_delete_ids[result - int(option_ids.size() + can_publish_ids.size())];
			do_remote_addon_delete(v, client, id);
			return;
		} else if(result >= int(option_ids.size())) {
			// Handle remote publishing.
			const std::string& id = can_publish_ids[result - int(option_ids.size())];
			do_remote_addon_publish(v, client, id, tracking[id].remote_version);
			return;
		}
	}

	std::vector<std::string> ids_to_install;
	std::vector<std::string> failed_titles;

	if(update_everything) {
		for(const std::string& id : option_ids) {
			if(tracking[id].state == ADDON_INSTALLED_UPGRADABLE) {
				ids_to_install.push_back(id);
			}
		}
	} else {
		assert(result >= 0 && size_t(result) < option_ids.size());
		last_addon_id = option_ids[result];
		ids_to_install.push_back(option_ids[result]);
	}

	for(const std::string& id : ids_to_install) {
		const addon_info& addon = addon_at(id, addons);

		addons_client::install_result res = client.install_addon_with_checks(addons, addon);

		wml_changed |= res.wml_changed; // take note if any wml_changes occurred
		if (res.outcome == addons_client::install_outcome::abort) {
			return; // the user aborted because of some issue encountered
		} else if (res.outcome == addons_client::install_outcome::failure) {
			failed_titles.push_back(addon.title); // we resolved dependencies, but fetching this particular addon failed.
		} else { // res.outcome == success
			wml_changed = true;
		}
	}

	std::string msg_title;
	std::string msg_text;

	// Use the Update terminology when using Update All or working with the
	// Upgradable add-ons view.
	const bool updating = update_everything || updates_only;

	if(ids_to_install.size() == 1 && failed_titles.empty()) {
		utils::string_map syms;
		syms["addon_title"] = addons[ids_to_install[0]].title;

		msg_title = !updating ? _("Add-on Installed") : _("Add-on Updated");
		msg_text = !updating ? _("The add-on '$addon_title|' has been successfully installed.") : _("The add-on '$addon_title|' has been successfully updated.");

		// Extra flags are so restore_background can be set. Remove when no longer necessary
		gui2::show_transient_message(v,
			msg_title, utils::interpolate_variables_into_string(msg_text, &syms), "", false, false, true);
	} else if(failed_titles.empty()) {
		msg_title = !updating ? _("Add-ons Installed") : _("Add-ons Updated");
		msg_text = !updating ? _("All add-ons installed successfully.") : _("All add-ons updated successfully.");

		gui2::show_transient_message(v, msg_title, msg_text, "", false, false, true);
	} else {
		msg_title = !updating ? _("Installation Failed") : _("Update Failed");
		msg_text = _n(
			"The following add-on could not be downloaded or installed successfully:",
			"The following add-ons could not be downloaded or installed successfully:",
			failed_titles.size());

		gui2::show_message(v, msg_title, msg_text + std::string("\n\n") + utils::bullet_list(failed_titles), gui2::dialogs::message::ok_button);
	}
}

bool addons_manager_ui(CVideo& v, const std::string& remote_address)
{
	bool stay_in_manager_ui = false;
	bool need_wml_cache_refresh = false;
	std::string last_addon_id;
	addons_filter_state filter;

	preferences::set_campaign_server(remote_address);

	try {
		do {
			if(need_wml_cache_refresh) {
				// The version info cache has gone stale because we installed/upgraded
				// an add-on in the previous iteration. Normally this cache is refreshed
				// along with all other caches, but we don't want to do all that here.
				// Thus, we refresh this specific cache when required, so that add-ons
				// are properly reported as installed/upgraded before leaving the
				// manager UI.
				refresh_addon_version_info_cache();
			}

			// TODO: don't create a new client instance each time we return to the UI,
			// but for that we need to make sure any pending network operations are canceled
			// whenever addons_client throws user_exit even before it gets destroyed
			addons_client client(v, remote_address);
			client.connect();

			addons_list addons;

			// BIG FAT TODO: get rid of the GUI1 addons manager. Just need to decide how best to decouple this.
			//if(gui2::new_widgets) {
				gui2::dialogs::addon_manager dlg(client);
				dlg.show(v);
				return true;
			//}

			if(!get_addons_list(client, addons)) {
				gui2::show_error_message(v, _("An error occurred while downloading the add-ons list from the server."));
				return need_wml_cache_refresh;
			}

			try {
				// Don't reconnect when switching between view modes.
				do {
					show_addons_manager_dialog(v, client, addons, last_addon_id, stay_in_manager_ui, need_wml_cache_refresh, filter);
				} while(filter.changed);
			} catch(const addons_client::user_exit&) {
				// Don't do anything; just go back to the addons manager UI
				// if the user cancels a download or other network operation
				// after fetching the add-ons list above.
				LOG_AC << "operation canceled by user; returning to add-ons manager\n";
			}
		} while(stay_in_manager_ui);
	} catch(const config::error& e) {
		ERR_CFG << "config::error thrown during transaction with add-on server; \""<< e.message << "\"" << std::endl;
		gui2::show_error_message(v, _("Network communication error."));
	} catch(const network_asio::error& e) {
		ERR_NET << "network_asio::error thrown during transaction with add-on server; \""<< e.what() << "\"" << std::endl;
		gui2::show_error_message(v, _("Remote host disconnected."));
	} catch(const filesystem::io_exception& e) {
		ERR_FS << "filesystem::io_exception thrown while installing an addon; \"" << e.what() << "\"" << std::endl;
		gui2::show_error_message(v, _("A problem occurred when trying to create the files necessary to install this add-on."));
	} catch(const invalid_pbl_exception& e) {
		ERR_CFG << "could not read .pbl file " << e.path << ": " << e.message << std::endl;

		utils::string_map symbols;
		symbols["path"] = e.path;
		symbols["msg"] = e.message;

		gui2::show_error_message(v,
			vgettext("A local file with add-on publishing information could not be read.\n\nFile: $path\nError message: $msg", symbols));
	} catch(wml_exception& e) {
		e.show(v);
	} catch(const addons_client::user_exit&) {
		LOG_AC << "initial connection canceled by user\n";
	} catch(const addons_client::invalid_server_address&) {
		gui2::show_error_message(v, _("The add-ons server address specified is not valid."));
	}

	return need_wml_cache_refresh;
}

bool uninstall_local_addons(CVideo& v)
{
	const std::string list_lead = "\n\n";

	const std::vector<std::string>& addons = installed_addons();

	if(addons.empty()) {
		gui2::show_error_message(v,
			_("You have no add-ons installed."));
		return false;
	}

	std::map<std::string, std::string> addon_titles_map;

	for(const std::string& id : addons) {
		std::string title;

		if(have_addon_install_info(id)) {
			// _info.cfg may have the add-on's title starting with 1.11.7,
			// if the add-on was downloading using the revised _info.cfg writer.
			config cfg;
			get_addon_install_info(id, cfg);

			const config& info_cfg = cfg.child("info");

			if(info_cfg) {
				title = info_cfg["title"].str();
			}
		}

		if(title.empty()) {
			// Transform the id into a title as a last resort.
			title = make_addon_title(id);
		}

		addon_titles_map[id] = title;
	}

	int res;

	std::vector<std::string> remove_ids;
	std::set<std::string> remove_names;

	do {
		gui2::dialogs::addon_uninstall_list dlg(addon_titles_map);
		dlg.show(v);

		remove_ids = dlg.selected_addons();
		if(remove_ids.empty()) {
			return false;
		}

		remove_names.clear();

		for(const std::string& id : remove_ids) {
			remove_names.insert(addon_titles_map[id]);
		}

		const std::string confirm_message = _n(
			"Are you sure you want to remove the following installed add-on?",
			"Are you sure you want to remove the following installed add-ons?",
			remove_ids.size()) + list_lead + utils::bullet_list(remove_names);

		res = gui2::show_message(v
				, _("Confirm")
				, confirm_message
				, gui2::dialogs::message::yes_no_buttons);
	} while (res != gui2::window::OK);

	std::set<std::string> failed_names, skipped_names, succeeded_names;

	for(const std::string& id : remove_ids) {
		const std::string& name = addon_titles_map[id];

		if(have_addon_pbl_info(id) || have_addon_in_vcs_tree(id)) {
			skipped_names.insert(name);
		} else if(remove_local_addon(id)) {
			succeeded_names.insert(name);
		} else {
			failed_names.insert(name);
		}
	}

	if(!skipped_names.empty()) {
		const std::string dlg_msg = _n(
			"The following add-on appears to have publishing or version control information stored locally, and will not be removed:",
			"The following add-ons appear to have publishing or version control information stored locally, and will not be removed:",
			skipped_names.size());

		gui2::show_error_message(
			v, dlg_msg + list_lead + utils::bullet_list(skipped_names));
	}

	if(!failed_names.empty()) {
		gui2::show_error_message(v, _n(
			"The following add-on could not be deleted properly:",
			"The following add-ons could not be deleted properly:",
			failed_names.size()) + list_lead + utils::bullet_list(failed_names));
	}

	if(!succeeded_names.empty()) {
		const std::string dlg_title =
			_n("Add-on Deleted", "Add-ons Deleted", succeeded_names.size());
		const std::string dlg_msg = _n(
			"The following add-on was successfully deleted:",
			"The following add-ons were successfully deleted:",
			succeeded_names.size());

		gui2::show_transient_message(
			v, dlg_title,
			dlg_msg + list_lead + utils::bullet_list(succeeded_names), "", false, false, true);

		return true;
	}

	return false;
}

} // end anonymous namespace

bool manage_addons(CVideo& v)
{
	static const int addon_download  = 0;
	// NOTE: the following two values are also known by WML, so don't change them.
	static const int addon_uninstall = 2;

	std::string host_name = preferences::campaign_server();
	const bool have_addons = !installed_addons().empty();

	gui2::dialogs::addon_connect addon_dlg(host_name, have_addons);
	addon_dlg.show(v);
	int res = addon_dlg.get_retval();

	if(res == gui2::window::OK) {
		res = addon_download;
	}

	switch(res) {
		case addon_download:
			return addons_manager_ui(v, host_name);
		case addon_uninstall:
			return uninstall_local_addons(v);
		default:
			return false;
	}
}

bool ad_hoc_addon_fetch_session(CVideo& v, const std::vector<std::string>& addon_ids)
{
	std::string remote_address = preferences::campaign_server();

	// These exception handlers copied from addon_manager_ui fcn above.
	try {

		addons_client client(v, remote_address);
		client.connect();

		addons_list addons;

		if(!get_addons_list(client, addons)) {
			gui2::show_error_message(v, _("An error occurred while downloading the add-ons list from the server."));
			return false;
		}

		bool return_value = true;
		for(const std::string & addon_id : addon_ids) {
			addons_list::const_iterator it = addons.find(addon_id);
			if(it != addons.end()) {
				const addon_info& addon = it->second;
				addons_client::install_result res = client.install_addon_with_checks(addons, addon);
				return_value = return_value && (res.outcome == addons_client::install_outcome::success);
			} else {
				utils::string_map symbols;
				symbols["addon_id"] = addon_id;
				gui2::show_error_message(v, vgettext("Could not find an add-on matching id $addon_id on the add-on server.", symbols));
				return_value = false;
			}
		}

		return return_value;

	} catch(const config::error& e) {
		ERR_CFG << "config::error thrown during transaction with add-on server; \""<< e.message << "\"" << std::endl;
		gui2::show_error_message(v, _("Network communication error."));
	} catch(const network_asio::error& e) {
		ERR_NET << "network_asio::error thrown during transaction with add-on server; \""<< e.what() << "\"" << std::endl;
		gui2::show_error_message(v, _("Remote host disconnected."));
	} catch(const filesystem::io_exception& e) {
		ERR_FS << "io_exception thrown while installing an addon; \"" << e.what() << "\"" << std::endl;
		gui2::show_error_message(v, _("A problem occurred when trying to create the files necessary to install this add-on."));
	} catch(const invalid_pbl_exception& e) {
		ERR_CFG << "could not read .pbl file " << e.path << ": " << e.message << std::endl;

		utils::string_map symbols;
		symbols["path"] = e.path;
		symbols["msg"] = e.message;

		gui2::show_error_message(v,
			vgettext("A local file with add-on publishing information could not be read.\n\nFile: $path\nError message: $msg", symbols));
	} catch(wml_exception& e) {
		e.show(v);
	} catch(const addons_client::user_exit&) {
		LOG_AC << "initial connection canceled by user\n";
	} catch(const addons_client::invalid_server_address&) {
		gui2::show_error_message(v, _("The add-ons server address specified is not valid."));
	}

	return false;
}
