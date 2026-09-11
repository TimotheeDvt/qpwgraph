// qpwgraph_matrix.cpp
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

#include "config.h"

#include "qpwgraph_matrix.h"

#include "qpwgraph_canvas.h"
#include "qpwgraph_node.h"
#include "qpwgraph_port.h"

#include "qpwgraph_pipewire.h"
#include "qpwgraph_alsamidi.h"

#include <QGraphicsScene>

#include <QVBoxLayout>

#include <QToolBar>
#include <QAction>

#include <QTableWidget>
#include <QHeaderView>

#include <QBrush>
#include <QFont>
#include <QPalette>

#include <QShowEvent>

#include <algorithm>


//----------------------------------------------------------------------------
// qpwgraph_matrix -- Connection matrix/grid alternate view.

// Constructor.
qpwgraph_matrix::qpwgraph_matrix (
	qpwgraph_canvas *canvas, QWidget *parent )
	: QWidget(parent), m_canvas(canvas),
		m_filter_toolbar(nullptr), m_table(nullptr), m_dirty(false)
{
	m_filter_toolbar = new QToolBar();
	m_filter_toolbar->setMovable(false);
	m_filter_toolbar->setFloatable(false);
	m_filter_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

	// Register the known port-types as toggle-able filters...
	addPortTypeFilter(qpwgraph_pipewire::audioPortType(), tr("Audio"));
	addPortTypeFilter(qpwgraph_pipewire::midiPortType(),  tr("MIDI"));
	addPortTypeFilter(qpwgraph_pipewire::midi2PortType(), tr("MIDI2"));
	addPortTypeFilter(qpwgraph_pipewire::videoPortType(), tr("Video"));
	addPortTypeFilter(qpwgraph_pipewire::otherPortType(), tr("Other"));
#ifdef CONFIG_ALSA_MIDI
	addPortTypeFilter(qpwgraph_alsamidi::midiPortType(),  tr("ALSA MIDI"));
#endif

	m_table = new QTableWidget();
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setAlternatingRowColors(false);
	m_table->setCornerButtonEnabled(false);
	m_table->setShowGrid(true);

	QHeaderView *hheader = m_table->horizontalHeader();
	hheader->setSectionResizeMode(QHeaderView::Interactive);
	hheader->setDefaultSectionSize(28);
	hheader->setMinimumSectionSize(24);

	QHeaderView *vheader = m_table->verticalHeader();
	vheader->setSectionResizeMode(QHeaderView::Interactive);
	vheader->setDefaultSectionSize(20);

	QObject::connect(m_table,
		SIGNAL(cellClicked(int, int)),
		SLOT(cellClicked(int, int)));

	QVBoxLayout *layout = new QVBoxLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(m_filter_toolbar);
	layout->addWidget(m_table, 1);
	QWidget::setLayout(layout);

	// Observe the canvas' own live graph notifications,
	// without touching/duplicating its (or the engine's)
	// connection logic in any way...
	QObject::connect(m_canvas,
		SIGNAL(added(qpwgraph_node *)),
		SLOT(added(qpwgraph_node *)));
	QObject::connect(m_canvas,
		SIGNAL(removed(qpwgraph_node *)),
		SLOT(removed(qpwgraph_node *)));
	QObject::connect(m_canvas,
		SIGNAL(connected(qpwgraph_port *, qpwgraph_port *)),
		SLOT(connected(qpwgraph_port *, qpwgraph_port *)));
	QObject::connect(m_canvas,
		SIGNAL(disconnected(qpwgraph_port *, qpwgraph_port *)),
		SLOT(disconnected(qpwgraph_port *, qpwgraph_port *)));
	QObject::connect(m_canvas,
		SIGNAL(renamed(qpwgraph_item *, const QString&)),
		SLOT(renamed(qpwgraph_item *, const QString&)));

	updateView();
}


// Destructor.
qpwgraph_matrix::~qpwgraph_matrix (void)
{
}


// Canvas accessor.
qpwgraph_canvas *qpwgraph_matrix::canvas (void) const
{
	return m_canvas;
}


// Full grid (re)build, on demand.
void qpwgraph_matrix::updateView (void)
{
	if (QWidget::isVisible())
		rebuild();
	else
		m_dirty = true;
}


// Canvas node life-cycle notifications.
void qpwgraph_matrix::added ( qpwgraph_node * )
{
	updateView();
}


void qpwgraph_matrix::removed ( qpwgraph_node * )
{
	updateView();
}


// Canvas rename notification.
void qpwgraph_matrix::renamed ( qpwgraph_item *, const QString& )
{
	updateView();
}


// Canvas port (dis)connection notifications.
//
// Note: these fire as soon as the (dis)connection is requested/queued,
// well before the actual PipeWire/ALSA link is confirmed by the engine
// (which only happens asynchronously, on the next registry round-trip).
// So the cell state is set optimistically here from the notification
// itself, not by (yet) querying qpwgraph_port::findConnect(); any later
// engine-side rejection or race gets reconciled on the next rebuild().
//
void qpwgraph_matrix::connected ( qpwgraph_port *port1, qpwgraph_port *port2 )
{
	updateCell(port1, port2, true);
}


void qpwgraph_matrix::disconnected ( qpwgraph_port *port1, qpwgraph_port *port2 )
{
	updateCell(port1, port2, false);
}


// Port-type filter toggle slot.
void qpwgraph_matrix::filterActionToggled ( bool on )
{
	QAction *action = qobject_cast<QAction *> (QObject::sender());
	if (action == nullptr)
		return;

	m_filter_types.insert(action->data().toUInt(), on);

	rebuild();
}


// Grid cell click slot.
void qpwgraph_matrix::cellClicked ( int row, int column )
{
	if (row < 0 || row >= m_row_ports.count())
		return;
	if (column < 0 || column >= m_col_ports.count())
		return;

	// Re-resolve to live ports, as the ones the grid was last
	// built from may since have been destroyed and recreated by
	// the engine's own periodic graph reconciliation (updateItems),
	// even for a logically unchanged port...
	qpwgraph_port *port1 = resolvePort(
		m_row_ports.at(row), qpwgraph_item::Output);
	qpwgraph_port *port2 = resolvePort(
		m_col_ports.at(column), qpwgraph_item::Input);

	if (port1 == nullptr || port2 == nullptr)
		return;
	if (port1->portType() != port2->portType())
		return;

	const bool is_connect = (port1->findConnect(port2) == nullptr);

	// Goes through the very same command/undo-stack, patchbay
	// bookkeeping and (dis)connected notifications the graph
	// canvas itself uses on a drag-connect gesture...
	if (m_canvas)
		m_canvas->connectPorts(port1, port2, is_connect);
}


// Deferred/on-demand grid (re)builder.
void qpwgraph_matrix::rebuild (void)
{
	m_dirty = false;

	m_table->setUpdatesEnabled(false);

	m_table->clearContents();
	m_table->setRowCount(0);
	m_table->setColumnCount(0);

	m_row_ports.clear();
	m_col_ports.clear();

	QList<qpwgraph_node *> nodes;

	if (m_canvas && m_canvas->scene()) {
		const QList<QGraphicsItem *>& items = m_canvas->scene()->items();
		foreach (QGraphicsItem *item, items) {
			if (item->type() == qpwgraph_node::Type) {
				qpwgraph_node *node = static_cast<qpwgraph_node *> (item);
				if (node && !(m_canvas->isFilterNodesEnabled()
						&& m_canvas->isFilterNodes(node->nodeName())))
					nodes.append(node);
			}
		}
	}

	std::sort(nodes.begin(), nodes.end(),
		[](qpwgraph_node *n1, qpwgraph_node *n2) -> bool
		{
			return QString::compare(
				n1->nodeName(), n2->nodeName(), Qt::CaseInsensitive) < 0;
		});

	// Live port pointers, valid only for the (synchronous) remainder
	// of this call -- never stored past this method's return; only
	// their lightweight PortRef address goes into the member lists.
	QList<qpwgraph_port *> row_live;
	QList<qpwgraph_port *> col_live;

	foreach (qpwgraph_node *node, nodes) {
		foreach (qpwgraph_port *port, node->ports()) {
			if (!isPortTypeEnabled(port->portType()))
				continue;
			if (port->isOutput())
				row_live.append(port);
			else
			if (port->isInput())
				col_live.append(port);
		}
	}

	m_table->setRowCount(row_live.count());
	m_table->setColumnCount(col_live.count());

	qpwgraph_node *last_row_node = nullptr;
	qpwgraph_node *last_col_node = nullptr;

	for (int row = 0; row < row_live.count(); ++row) {
		qpwgraph_port *port = row_live.at(row);
		qpwgraph_node *node = port->portNode();
		const QString label = node->nodeName()
			+ "  •  " + port->portName();
		QTableWidgetItem *hitem = new QTableWidgetItem(label);
		hitem->setToolTip(label);
		if (node != last_row_node) {
			QFont font = hitem->font();
			font.setBold(true);
			hitem->setFont(font);
			last_row_node = node;
		}
		m_table->setVerticalHeaderItem(row, hitem);
		m_row_ports.append(refOf(port));
	}

	for (int col = 0; col < col_live.count(); ++col) {
		qpwgraph_port *port = col_live.at(col);
		qpwgraph_node *node = port->portNode();
		const QString label = node->nodeName()
			+ "  •  " + port->portName();
		QTableWidgetItem *hitem = new QTableWidgetItem(label);
		hitem->setToolTip(label);
		if (node != last_col_node) {
			QFont font = hitem->font();
			font.setBold(true);
			hitem->setFont(font);
			last_col_node = node;
		}
		m_table->setHorizontalHeaderItem(col, hitem);
		m_col_ports.append(refOf(port));
	}

	for (int row = 0; row < row_live.count(); ++row) {
		qpwgraph_port *port1 = row_live.at(row);
		for (int col = 0; col < col_live.count(); ++col) {
			qpwgraph_port *port2 = col_live.at(col);
			QTableWidgetItem *item = new QTableWidgetItem();
			item->setTextAlignment(Qt::AlignCenter);
			if (port1->portType() != port2->portType()) {
				item->setFlags(item->flags()
					& ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
				item->setBackground(QBrush(
					m_table->palette().color(QPalette::Mid),
					Qt::Dense7Pattern));
			} else {
				item->setFlags((item->flags() | Qt::ItemIsEnabled)
					& ~Qt::ItemIsSelectable);
				if (port1->findConnect(port2))
					item->setBackground(m_canvas->portTypeColor(port1->portType()));
			}
			m_table->setItem(row, col, item);
		}
	}

	m_table->setUpdatesEnabled(true);
}


// Single-cell state updater (no full rebuild).
void qpwgraph_matrix::updateCell (
	qpwgraph_port *port1, qpwgraph_port *port2, bool is_connect )
{
	const int row = m_row_ports.indexOf(refOf(port1));
	if (row < 0)
		return;
	const int col = m_col_ports.indexOf(refOf(port2));
	if (col < 0)
		return;

	QTableWidgetItem *item = m_table->item(row, col);
	if (item == nullptr)
		return;

	if (is_connect)
		item->setBackground(m_canvas->portTypeColor(port1->portType()));
	else
		item->setBackground(QBrush());
}


// Address of a (still live) port.
qpwgraph_matrix::PortRef qpwgraph_matrix::refOf ( qpwgraph_port *port ) const
{
	PortRef ref{};

	if (port) {
		qpwgraph_node *node = port->portNode();
		if (node) {
			ref.node_id   = node->nodeId();
			ref.node_type = node->nodeType();
		}
		ref.port_id   = port->portId();
		ref.port_type = port->portType();
	}

	return ref;
}


// Re-resolve a port address into a live port, if still current.
qpwgraph_port *qpwgraph_matrix::resolvePort (
	const PortRef& ref, qpwgraph_item::Mode mode ) const
{
	if (m_canvas == nullptr)
		return nullptr;

	qpwgraph_node *node = m_canvas->findNode(ref.node_id, mode, ref.node_type);
	if (node == nullptr)
		node = m_canvas->findNode(ref.node_id, qpwgraph_item::Duplex, ref.node_type);
	if (node == nullptr)
		return nullptr;

	return node->findPort(ref.port_id, mode, ref.port_type);
}


// Port-type filter inquirer.
bool qpwgraph_matrix::isPortTypeEnabled ( uint port_type ) const
{
	return m_filter_types.value(port_type, true);
}


// Register a filter toggle-action for a port-type, if not already.
void qpwgraph_matrix::addPortTypeFilter (
	uint port_type, const QString& text )
{
	if (m_filter_actions.contains(port_type))
		return;

	QAction *action = new QAction(text, m_filter_toolbar);
	action->setCheckable(true);
	action->setChecked(true);
	action->setData(port_type);

	QObject::connect(action,
		SIGNAL(toggled(bool)),
		SLOT(filterActionToggled(bool)));

	m_filter_toolbar->addAction(action);
	m_filter_actions.insert(port_type, action);
	m_filter_types.insert(port_type, true);
}


// Widget event handler.
void qpwgraph_matrix::showEvent ( QShowEvent *event )
{
	QWidget::showEvent(event);

	if (m_dirty)
		rebuild();
}


// end of qpwgraph_matrix.cpp
