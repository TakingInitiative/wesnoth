/*
   Copyright (C) 2008 - 2016 by Mark de Wever <koraq@xs4all.nl>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#pragma once

#include "gui/dialogs/modal_dialog.hpp"
#include "sdl/rect.hpp"

#include <boost/dynamic_bitset.hpp>

class config;

namespace gui2
{
namespace dialogs
{
///Used by the menu_button widget.
class drop_down_menu : public modal_dialog
{
public:
	drop_down_menu(SDL_Rect button_pos, const std::vector<config>& items, int selected_item, bool use_markup, bool keep_open)
		: items_(items)
		, toggle_states_()
		, button_pos_(button_pos)
		, selected_item_(selected_item)
		, use_markup_(use_markup)
		, keep_open_(keep_open)
	{
		set_restore(true);
	}

	int selected_item() const
	{
		return selected_item_;
	}

	boost::dynamic_bitset<> get_toggle_states() const
	{
		return toggle_states_;
	}

private:
	/** Configuration of rach row. */
	std::vector<config> items_;

	/** If a toggle button widget is present, returns the toggled state of each row's button. */
	boost::dynamic_bitset<> toggle_states_;

	/**
	 * The screen location of the menu_button button that triggered this droplist.
	 * Note: we don't adjust the location of this dialog to when resizing the window.
	 * Instead this dialog automatically closes itself on resizing.
	 */
	SDL_Rect button_pos_;

	int selected_item_;

	bool use_markup_;

	/**
	 * Whether to keep this dialog open after a click occurs not handled by special exceptions
	 * such as scrollbars and toggle butons.
	 */
	bool keep_open_;

	/** Inherited from modal_dialog, implemented by REGISTER_DIALOG. */
	virtual const std::string& window_id() const;

	/** Inherited from modal_dialog. */
	void pre_show(window& window);

	/** Inherited from modal_dialog. */
	void post_show(window& window);
};

} // namespace dialogs
} // namespace gui2
