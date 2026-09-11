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

#include "qpwgraph_item.h"


// Forward decls.
class qpwgraph_canvas;
class qpwgraph_node;
class qpwgraph_port;

class QTableWidget;
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

	// Canvas port (dis)connection notifications.
	void connected(qpwgraph_port *port1, qpwgraph_port *port2);
	void disconnected(qpwgraph_port *port1, qpwgraph_port *port2);

	// Canvas rename notification.
	void renamed(qpwgraph_item *item, const QString& name);

	// Port-type filter toggle slot.
	void filterActionToggled(bool on);

	// Grid cell click slot.
	void cellClicked(int row, int column);

protected:

	// Deferred/on-demand grid (re)builder.
	void rebuild();

	// Single-cell state updater (no full rebuild).
	void updateCell(qpwgraph_port *port1, qpwgraph_port *port2, bool is_connect);

	// Port-type filter inquirer.
	bool isPortTypeEnabled(uint port_type) const;

	// Register a filter toggle-action for a port-type, if not already.
	void addPortTypeFilter(uint port_type, const QString& text);

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
		uint node_id;
		uint node_type;
		uint port_id;
		uint port_type;

		bool operator==(const PortRef& other) const
		{
			return node_id   == other.node_id
				&& node_type == other.node_type
				&& port_id   == other.port_id
				&& port_type == other.port_type;
		}
	};

	// Address of a (still live) port.
	PortRef refOf(qpwgraph_port *port) const;

	// Re-resolve a port address into a live port, if still current.
	qpwgraph_port *resolvePort(
		const PortRef& ref, qpwgraph_item::Mode mode) const;

private:

	// Instance variables.
	qpwgraph_canvas *m_canvas;

	QToolBar     *m_filter_toolbar;
	QTableWidget *m_table;

	// Current row/column port mapping (outputs/inputs, resp.)
	QList<PortRef> m_row_ports;
	QList<PortRef> m_col_ports;

	// Port-type filter state and actions.
	QHash<uint, bool>     m_filter_types;
	QHash<uint, QAction *> m_filter_actions;

	// Deferred rebuild flag (while not visible/current).
	bool m_dirty;
};


#endif	// __qpwgraph_matrix_h

// end of qpwgraph_matrix.h
