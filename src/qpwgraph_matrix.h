// qpwgraph_matrix.h
//
/****************************************************************************
   Copyright (C) 2021-2026, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*****************************************************************************/

#ifndef __qpwgraph_matrix_h
#define __qpwgraph_matrix_h

#include <QWidget>
#include <QList>
#include <QHash>
#include <QColor>

#include "qpwgraph_item.h"


// Forward decls.
class qpwgraph_canvas;
class qpwgraph_node;
class qpwgraph_port;

class QToolBar;
class QAction;

class QShowEvent;


//----------------------------------------------------------------------------
// qpwgraph_matrix -- Connection matrix/grid alternate view.

class qpwgraph_matrix : public QWidget
{
	Q_OBJECT

public:

	// Constructor.
	qpwgraph_matrix(qpwgraph_canvas *canvas, QWidget *parent = nullptr);

	// Destructor.
	~qpwgraph_matrix();

	// Canvas accessor.
	qpwgraph_canvas *canvas() const;

public slots:

	// Full grid (re)build, on demand.
	void updateView();

protected slots:

	// Canvas node life-cycle notifications.
	void added(qpwgraph_node *node);
	void removed(qpwgraph_node *node);

	// Canvas rename notification.
	void renamed(qpwgraph_item *item, const QString& name);

	// Port-type filter toggle slot.
	void filterActionToggled(bool on);

protected:

	// Deferred/on-demand grid (re)builder.
	void rebuild();

	// Port-type filter inquirer.
	bool isPortTypeEnabled(uint port_type) const;

	// Register a filter toggle-action for a port-type, if not already.
	void addPortTypeFilter(uint port_type, const QString& text, bool enabled);

	// Widget event handler.
	void showEvent(QShowEvent *event);

	// Lightweight node:port address, as a stand-in for a live
	// qpwgraph_port pointer that may not stay valid across event-loop
	// iterations (nodes/ports routinely get destroyed and recreated
	// by the engine's own periodic graph reconciliation, even when
	// the same logical port persists) -- mirrors the very same
	// id-based (re)resolution scheme qpwgraph_connect_command already
	// relies on for its own undo/redo command stack.
	struct PortRef
	{
		uint node_id = 0;
		uint node_type = 0;
		uint port_id = 0;
		uint port_type = 0;

		bool operator==(const PortRef& other) const
		{
			return node_id   == other.node_id
				&& node_type == other.node_type
				&& port_id   == other.port_id
				&& port_type == other.port_type;
		}
	};

	// A single row/column line of the grid: either a real, connectable
	// port (is_port true) or the one-line stand-in for a collapsed
	// node group (is_port false, no specific port). The first line of
	// every node group (collapsed or not) is marked group_first and
	// carries the click-to-collapse/expand affordance.
	struct Line
	{
		PortRef port;
		QString node_name;
		QString port_name;
		uint    node_id = 0;
		uint    node_type = 0;
		bool    is_port = false;
		bool    group_first = false;
		bool    collapsed = false;
	};

	// Address of a (still live) port.
	PortRef refOf(qpwgraph_port *port) const;

	// Re-resolve a port address into a live port, if still current.
	qpwgraph_port *resolvePort(
		const PortRef& ref, qpwgraph_item::Mode mode) const;

	// Node group collapse-state key/helpers.
	static quint64 nodeKey(uint node_id, uint node_type);

	// Effective collapse state for a node group: an explicit user
	// toggle (m_row/col_collapsed_user) always wins; absent that, a
	// group with no active connection on any of its (filtered) ports
	// on this axis auto-collapses, so a busy grid opens decluttered
	// by default without hiding anything the user has deliberately
	// expanded (or re-collapsed) by hand.
	bool isRowCollapsed(uint node_id, uint node_type, bool auto_collapsed) const;
	bool isColCollapsed(uint node_id, uint node_type, bool auto_collapsed) const;

	// Toggle a node group's collapse state (called by the Grid on a
	// group-header line click) and rebuild. This always records an
	// explicit user override, taking precedence over the auto-collapse
	// heuristic from then on.
	void toggleRowGroup(uint node_id, uint node_type);
	void toggleColGroup(uint node_id, uint node_type);

	// Cell activation (called by the Grid on a port x port cell click).
	void activateCell(int row, int col);

	// Cell paint/hit-test info, resolved fresh (never cached) from the
	// row/col PortRefs -- the Grid may repaint at any arbitrary time
	// (a scroll, an expose, the periodic sync timer), so, just like
	// activateCell, this must never dereference a stale port pointer.
	struct CellInfo
	{
		bool   valid = false;
		bool   connected = false;
		bool   incompatible = false;
		QColor color;
	};

	CellInfo cellInfo(int row, int col) const;

	// Display text for a row/column line (shared by layout measurement
	// and painting, so they can never disagree).
	QString lineLabel(const Line& line) const;

	// Hover tooltip text for a grid cell ("input -> output"), empty
	// if the cell isn't a real port x port pair (eg. a folder-heading
	// row/column).
	QString cellTooltip(int row, int col) const;

private:

	// Custom-painted grid/header view (outputs down the left, inputs
	// diagonally along the bottom, Ardour-style); declared here, defined
	// in the .cpp -- mirrors the qpwgraph_thumb::View nested-class idiom
	// already used elsewhere in this codebase.
	class Grid;
	friend class Grid;

	// Instance variables.
	qpwgraph_canvas *m_canvas;

	QToolBar *m_filter_toolbar;
	Grid     *m_grid;

	// Current row/column line mapping (outputs/inputs, resp.)
	QList<Line> m_row_lines;
	QList<Line> m_col_lines;

	// Explicit user collapse/expand overrides, keyed by
	// nodeKey(node_id, node_type); absent a key, the group's collapse
	// state auto-follows whether it currently has any connection.
	QHash<quint64, bool> m_row_collapsed_user;
	QHash<quint64, bool> m_col_collapsed_user;

	// Port-type filter state and actions.
	QHash<uint, bool>     m_filter_types;
	QHash<uint, QAction *> m_filter_actions;

	// Deferred rebuild flag (while not visible/current).
	bool m_dirty;
};


#endif	// __qpwgraph_matrix_h

// end of qpwgraph_matrix.h
