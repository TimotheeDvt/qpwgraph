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

#include <QAbstractScrollArea>
#include <QScrollBar>

#include <QPainter>
#include <QFontMetrics>

#include <QMouseEvent>
#include <QResizeEvent>
#include <QHelpEvent>
#include <QToolTip>

#include <QTimer>
#include <QtMath>

#include <QShowEvent>

#include <algorithm>


//----------------------------------------------------------------------------
// qpwgraph_matrix::Grid -- Custom-painted grid/header view.
//
// Outputs are listed down the left (horizontal labels, as before);
// inputs run along the bottom, rotated at an angle (Ardour-style).
// Row/column headers stay pinned to the viewport edges as the grid
// scrolls beneath/beside them, like a spreadsheet's frozen headers.

class qpwgraph_matrix::Grid : public QAbstractScrollArea
{
public:

	// Constructor.
	Grid(qpwgraph_matrix *matrix)
		: QAbstractScrollArea(nullptr), m_matrix(matrix),
			m_cell_w(20), m_cell_h(20),
			m_row_header_w(120), m_col_footer_h(100),
			m_press_region(None), m_press_index1(-1), m_press_index2(-1)
	{
		QAbstractScrollArea::viewport()->setMouseTracking(true);
		QAbstractScrollArea::setFocusPolicy(Qt::NoFocus);
		QAbstractScrollArea::setFrameShape(QFrame::NoFrame);

		// Connection state (which cells are lit up) can change from
		// underneath us at any time, without any canvas-level signal
		// to hook (only whole nodes get added/removed notifications);
		// a cheap periodic repaint keeps cells honest -- paint always
		// re-resolves ports fresh, so this is just a redraw, not a
		// rebuild.
		QTimer *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] {
			if (QAbstractScrollArea::isVisible())
				QAbstractScrollArea::viewport()->update();
		});
		timer->start(500);
	}

	// Recompute header/footer extents and scrollbar ranges, then redraw.
	void updateLayout()
	{
		const QFontMetrics fm(font());

		int row_w = 0;
		foreach (const Line& line, m_matrix->m_row_lines)
			row_w = qMax(row_w, fm.horizontalAdvance(m_matrix->lineLabel(line)));
		m_row_header_w = qBound(80, row_w + 16, 280);

		int col_w = 0;
		foreach (const Line& line, m_matrix->m_col_lines)
			col_w = qMax(col_w, fm.horizontalAdvance(m_matrix->lineLabel(line)));
		const qreal footer_extent = col_w * qSin(qDegreesToRadians(kFooterAngle));
		m_col_footer_h = qBound(60, int(footer_extent) + 20, 220);

		updateScrollBars();

		QAbstractScrollArea::viewport()->update();
	}

protected:

	// Grid painter.
	void paintEvent(QPaintEvent *)
	{
		QPainter painter(QAbstractScrollArea::viewport());
		painter.setRenderHint(QPainter::Antialiasing, true);

		const QPalette& pal = QAbstractScrollArea::palette();
		const QColor text_color = pal.color(QPalette::Text);
		const QColor line_color = pal.color(QPalette::Mid);
		const QColor head_bg    = pal.color(QPalette::Button);
		const QColor grid_bg    = pal.color(QPalette::Base);

		const QRect vp = QAbstractScrollArea::viewport()->rect();
		painter.fillRect(vp, grid_bg);

		const int rows = m_matrix->m_row_lines.count();
		const int cols = m_matrix->m_col_lines.count();

		const int grid_x0 = m_row_header_w;
		const int grid_y1 = gridBottom();
		const int grid_w  = qMax(0, vp.width() - grid_x0);

		const int hoff = QAbstractScrollArea::horizontalScrollBar()->value();
		const int voff = QAbstractScrollArea::verticalScrollBar()->value();

		const QFont base_font = painter.font();
		const QFontMetrics fm(base_font);

		const int row0 = qMax(0, voff / m_cell_h);
		const int row1 = qMin(rows, (voff + grid_y1) / m_cell_h + 1);
		const int col0 = qMax(0, hoff / m_cell_w);
		const int col1 = qMin(cols, (hoff + grid_w) / m_cell_w + 1);

		// --- grid cells ---
		painter.save();
		painter.setClipRect(QRect(grid_x0, 0, grid_w, grid_y1));
		for (int row = row0; row < row1; ++row) {
			const int y = row * m_cell_h - voff;
			for (int col = col0; col < col1; ++col) {
				const qpwgraph_matrix::CellInfo info = m_matrix->cellInfo(row, col);
				if (!info.valid)
					continue; // folder-heading row/column -- no cell here at all
				const int x = grid_x0 + col * m_cell_w - hoff;
				const QRect cell_rect(x, y, m_cell_w, m_cell_h);
				if (info.incompatible) {
					painter.fillRect(cell_rect.adjusted(1, 1, -1, -1),
						QBrush(line_color, Qt::Dense7Pattern));
				} else if (info.connected) {
					painter.fillRect(cell_rect.adjusted(1, 1, -1, -1), info.color);
				}
				painter.setPen(line_color);
				painter.drawRect(cell_rect.adjusted(0, 0, -1, -1));
			}
		}
		painter.restore();

		// --- row headers (left, fixed x, scrolls with rows) ---
		painter.save();
		painter.setClipRect(QRect(0, 0, m_row_header_w, grid_y1));
		painter.fillRect(QRect(0, 0, m_row_header_w, grid_y1), head_bg);
		painter.setPen(text_color);
		for (int row = row0; row < row1; ++row) {
			const int y = row * m_cell_h - voff;
			const Line& line = m_matrix->m_row_lines.at(row);
			QFont font = base_font;
			font.setBold(line.group_first);
			painter.setFont(font);
			const QFontMetrics lfm(font);
			const QString text = lfm.elidedText(
				m_matrix->lineLabel(line), Qt::ElideRight, m_row_header_w - 8);
			painter.drawText(QRect(4, y, m_row_header_w - 6, m_cell_h),
				Qt::AlignVCenter | Qt::AlignLeft, text);
		}
		painter.setPen(line_color);
		painter.drawLine(m_row_header_w, 0, m_row_header_w, grid_y1);
		painter.restore();

		// --- column footers (bottom, fixed y, scrolls with cols) ---
		painter.save();
		painter.setClipRect(QRect(grid_x0, grid_y1, grid_w, m_col_footer_h));
		painter.fillRect(QRect(grid_x0, grid_y1, grid_w, m_col_footer_h), head_bg);
		painter.setPen(text_color);
		const qreal max_len = qMax(qreal(20),
			m_col_footer_h / qSin(qDegreesToRadians(kFooterAngle)) - 8);
		for (int col = col0; col < col1; ++col) {
			const int x = grid_x0 + col * m_cell_w - hoff + m_cell_w / 2;
			const Line& line = m_matrix->m_col_lines.at(col);
			QFont font = base_font;
			font.setBold(line.group_first);
			painter.setFont(font);
			const QFontMetrics lfm(font);
			const QString text = lfm.elidedText(
				m_matrix->lineLabel(line), Qt::ElideRight, int(max_len));
			painter.save();
			painter.translate(x, grid_y1 + m_col_footer_h - 4);
			painter.rotate(-kFooterAngle);
			painter.drawText(QPoint(0, 0), text);
			painter.restore();
		}
		painter.setPen(line_color);
		painter.drawLine(grid_x0, grid_y1, vp.width(), grid_y1);
		painter.restore();

		// --- corner filler (bottom-left) ---
		const QRect corner_rect(0, grid_y1, m_row_header_w, m_col_footer_h);
		painter.fillRect(corner_rect, head_bg);
		painter.setPen(line_color);
		painter.drawRect(corner_rect.adjusted(0, 0, -1, -1));
	}

	void resizeEvent(QResizeEvent *event)
	{
		QAbstractScrollArea::resizeEvent(event);
		updateScrollBars();
	}

	void scrollContentsBy(int, int)
	{
		QAbstractScrollArea::viewport()->update();
	}

	void mousePressEvent(QMouseEvent *event)
	{
		if (event->button() == Qt::LeftButton) {
			const HitResult hr = hitTest(event->pos());
			m_press_region = hr.region;
			m_press_index1 = hr.index1;
			m_press_index2 = hr.index2;
		}
		QAbstractScrollArea::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event)
	{
		if (event->button() == Qt::LeftButton && m_press_region != None) {
			const HitResult hr = hitTest(event->pos());
			if (hr.region == m_press_region
				&& hr.index1 == m_press_index1
				&& hr.index2 == m_press_index2) {
				switch (hr.region) {
				case RowHeader: {
					const Line& line = m_matrix->m_row_lines.at(hr.index1);
					if (line.group_first)
						m_matrix->toggleRowGroup(line.node_id, line.node_type);
					break;
				}
				case ColFooter: {
					const Line& line = m_matrix->m_col_lines.at(hr.index1);
					if (line.group_first)
						m_matrix->toggleColGroup(line.node_id, line.node_type);
					break;
				}
				case Cell:
					m_matrix->activateCell(hr.index1, hr.index2);
					break;
				default:
					break;
				}
			}
		}
		m_press_region = None;
		QAbstractScrollArea::mouseReleaseEvent(event);
	}

	bool viewportEvent(QEvent *event)
	{
		if (event->type() == QEvent::ToolTip) {
			QHelpEvent *help = static_cast<QHelpEvent *> (event);
			const HitResult hr = hitTest(help->pos());
			QString text;
			if (hr.region == RowHeader)
				text = m_matrix->lineLabel(m_matrix->m_row_lines.at(hr.index1)).trimmed();
			else
			if (hr.region == ColFooter)
				text = m_matrix->lineLabel(m_matrix->m_col_lines.at(hr.index1)).trimmed();
			else
			if (hr.region == Cell)
				text = m_matrix->cellTooltip(hr.index1, hr.index2);
			if (text.isEmpty())
				QToolTip::hideText();
			else
				QToolTip::showText(help->globalPos(), text, this);
			return true;
		}
		return QAbstractScrollArea::viewportEvent(event);
	}

private:

	// Header/footer angle (degrees), diagonal destination labels.
	static constexpr qreal kFooterAngle = 60.0;

	enum Region { None = 0, RowHeader, ColFooter, Cell };

	struct HitResult { Region region; int index1; int index2; };

	HitResult hitTest(const QPoint& pos) const
	{
		HitResult hr{ None, -1, -1 };

		const int grid_y1 = gridBottom();
		const int hoff = QAbstractScrollArea::horizontalScrollBar()->value();
		const int voff = QAbstractScrollArea::verticalScrollBar()->value();

		const int rows = m_matrix->m_row_lines.count();
		const int cols = m_matrix->m_col_lines.count();

		if (pos.x() < m_row_header_w) {
			if (pos.y() < 0 || pos.y() >= grid_y1)
				return hr;
			const int row = (pos.y() + voff) / m_cell_h;
			if (row >= 0 && row < rows) {
				hr.region = RowHeader;
				hr.index1 = row;
			}
			return hr;
		}

		if (pos.y() >= grid_y1) {
			if (pos.y() >= grid_y1 + m_col_footer_h)
				return hr;
			const int col = (pos.x() - m_row_header_w + hoff) / m_cell_w;
			if (col >= 0 && col < cols) {
				hr.region = ColFooter;
				hr.index1 = col;
			}
			return hr;
		}

		const int row = (pos.y() + voff) / m_cell_h;
		const int col = (pos.x() - m_row_header_w + hoff) / m_cell_w;
		if (row >= 0 && row < rows && col >= 0 && col < cols) {
			hr.region = Cell;
			hr.index1 = row;
			hr.index2 = col;
		}
		return hr;
	}

	// Screen-y of the boundary between the cell grid and the footer:
	// glued right below the actual row content (like Ardour), not
	// stretched down to the viewport edge when there's only a short
	// list of rows -- but still pinned to the viewport bottom (usual
	// frozen-footer behaviour) once there's enough content to scroll.
	int gridBottom() const
	{
		const QRect vp = QAbstractScrollArea::viewport()->rect();
		const int avail_h = qMax(0, vp.height() - m_col_footer_h);
		const int content_h = m_matrix->m_row_lines.count() * m_cell_h;
		const int voff = QAbstractScrollArea::verticalScrollBar()->value();
		return qMin(avail_h, qMax(0, content_h - voff));
	}

	void updateScrollBars()
	{
		const QRect vp = QAbstractScrollArea::viewport()->rect();
		const int grid_w = qMax(0, vp.width()  - m_row_header_w);
		const int grid_h = qMax(0, vp.height() - m_col_footer_h);

		const int content_w = m_matrix->m_col_lines.count() * m_cell_w;
		const int content_h = m_matrix->m_row_lines.count() * m_cell_h;

		QAbstractScrollArea::horizontalScrollBar()->setRange(0, qMax(0, content_w - grid_w));
		QAbstractScrollArea::horizontalScrollBar()->setPageStep(qMax(1, grid_w));
		QAbstractScrollArea::verticalScrollBar()->setRange(0, qMax(0, content_h - grid_h));
		QAbstractScrollArea::verticalScrollBar()->setPageStep(qMax(1, grid_h));
	}

	// Instance variables.
	qpwgraph_matrix *m_matrix;

	int m_cell_w;
	int m_cell_h;
	int m_row_header_w;
	int m_col_footer_h;

	Region m_press_region;
	int m_press_index1;
	int m_press_index2;
};


//----------------------------------------------------------------------------
// qpwgraph_matrix -- Connection matrix/grid alternate view.

// Constructor.
qpwgraph_matrix::qpwgraph_matrix (
	qpwgraph_canvas *canvas, QWidget *parent )
	: QWidget(parent), m_canvas(canvas),
		m_filter_toolbar(nullptr), m_grid(nullptr), m_dirty(false)
{
	m_filter_toolbar = new QToolBar();
	m_filter_toolbar->setMovable(false);
	m_filter_toolbar->setFloatable(false);
	m_filter_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

	// Register the known port-types as toggle-able filters...
	addPortTypeFilter(qpwgraph_pipewire::audioPortType(), tr("Audio"), true);
	addPortTypeFilter(qpwgraph_pipewire::midiPortType(),  tr("MIDI"), false);
	addPortTypeFilter(qpwgraph_pipewire::midi2PortType(), tr("MIDI2"), false);
	addPortTypeFilter(qpwgraph_pipewire::videoPortType(), tr("Video"), false);
	addPortTypeFilter(qpwgraph_pipewire::otherPortType(), tr("Other"), false);
#ifdef CONFIG_ALSA_MIDI
	addPortTypeFilter(qpwgraph_alsamidi::midiPortType(),  tr("ALSA MIDI"), false);
#endif

	m_grid = new Grid(this);

	QVBoxLayout *layout = new QVBoxLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(m_filter_toolbar);
	layout->addWidget(m_grid, 1);
	QWidget::setLayout(layout);

	// Observe the canvas' own live graph notifications, without
	// touching/duplicating its (or the engine's) connection logic
	// in any way...
	QObject::connect(m_canvas,
		SIGNAL(added(qpwgraph_node *)),
		SLOT(added(qpwgraph_node *)));
	QObject::connect(m_canvas,
		SIGNAL(removed(qpwgraph_node *)),
		SLOT(removed(qpwgraph_node *)));
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


// Port-type filter toggle slot.
void qpwgraph_matrix::filterActionToggled ( bool on )
{
	QAction *action = qobject_cast<QAction *> (QObject::sender());
	if (action == nullptr)
		return;

	m_filter_types.insert(action->data().toUInt(), on);

	rebuild();
}


// Deferred/on-demand grid (re)builder.
void qpwgraph_matrix::rebuild (void)
{
	m_dirty = false;

	m_row_lines.clear();
	m_col_lines.clear();

	// Per-node port gathering, done once; the row and column axes are
	// then ordered independently (see below) since a node's outputs
	// and inputs may not agree on whether the group is "empty".
	struct NodeInfo
	{
		qpwgraph_node *node;
		QList<qpwgraph_port *> row_ports;
		QList<qpwgraph_port *> col_ports;
		bool row_has_link = false;
		bool col_has_link = false;
	};

	QList<NodeInfo> infos;

	if (m_canvas && m_canvas->scene()) {
		const QList<QGraphicsItem *>& items = m_canvas->scene()->items();
		foreach (QGraphicsItem *item, items) {
			if (item->type() != qpwgraph_node::Type)
				continue;
			qpwgraph_node *node = static_cast<qpwgraph_node *> (item);
			if (node == nullptr)
				continue;
			if (m_canvas->isFilterNodesEnabled()
				&& m_canvas->isFilterNodes(node->nodeName()))
				continue;
			NodeInfo info;
			info.node = node;
			foreach (qpwgraph_port *port, node->ports()) {
				if (!isPortTypeEnabled(port->portType()))
					continue;
				if (port->isOutput()) {
					info.row_ports.append(port);
					if (!port->connects().isEmpty())
						info.row_has_link = true;
				} else
				if (port->isInput()) {
					info.col_ports.append(port);
					if (!port->connects().isEmpty())
						info.col_has_link = true;
				}
			}
			infos.append(info);
		}
	}

	// Output rows: empty groups first, linked ones pushed to the
	// bottom (each bucket alphabetical). Input columns: the reverse
	// -- linked groups first, empty ones after.
	QList<int> row_order, col_order;
	for (int i = 0; i < infos.count(); ++i) {
		if (!infos.at(i).row_ports.isEmpty())
			row_order.append(i);
		if (!infos.at(i).col_ports.isEmpty())
			col_order.append(i);
	}

	std::sort(row_order.begin(), row_order.end(),
		[&infos](int a, int b) -> bool
		{
			const NodeInfo& ia = infos.at(a);
			const NodeInfo& ib = infos.at(b);
			if (ia.row_has_link != ib.row_has_link)
				return !ia.row_has_link;
			return QString::compare(ia.node->nodeName(),
				ib.node->nodeName(), Qt::CaseInsensitive) < 0;
		});

	std::sort(col_order.begin(), col_order.end(),
		[&infos](int a, int b) -> bool
		{
			const NodeInfo& ia = infos.at(a);
			const NodeInfo& ib = infos.at(b);
			if (ia.col_has_link != ib.col_has_link)
				return ia.col_has_link;
			return QString::compare(ia.node->nodeName(),
				ib.node->nodeName(), Qt::CaseInsensitive) < 0;
		});

	foreach (int i, row_order) {
		const NodeInfo& info = infos.at(i);
		qpwgraph_node *node = info.node;

		// Folder/group heading -- never a real port, never
		// clickable, whether collapsed or expanded.
		const bool row_collapsed = isRowCollapsed(
			node->nodeId(), node->nodeType(), !info.row_has_link);
		Line header;
		header.node_name = node->nodeName();
		header.node_id = node->nodeId();
		header.node_type = node->nodeType();
		header.group_first = true;
		header.collapsed = row_collapsed;
		m_row_lines.append(header);

		if (!row_collapsed) {
			QStringList names;
			foreach (qpwgraph_port *port, info.row_ports)
				names.append(port->portName());
			const QString prefix = commonPrefix(names);
			foreach (qpwgraph_port *port, info.row_ports) {
				Line line;
				line.node_name = node->nodeName();
				line.node_id = node->nodeId();
				line.node_type = node->nodeType();
				line.is_port = true;
				line.port = refOf(port);
				line.port_name = port->portName().mid(prefix.length());
				m_row_lines.append(line);
			}
		}
	}

	foreach (int i, col_order) {
		const NodeInfo& info = infos.at(i);
		qpwgraph_node *node = info.node;

		const bool col_collapsed = isColCollapsed(
			node->nodeId(), node->nodeType(), !info.col_has_link);
		Line header;
		header.node_name = node->nodeName();
		header.node_id = node->nodeId();
		header.node_type = node->nodeType();
		header.group_first = true;
		header.collapsed = col_collapsed;
		m_col_lines.append(header);

		if (!col_collapsed) {
			QStringList names;
			foreach (qpwgraph_port *port, info.col_ports)
				names.append(port->portName());
			const QString prefix = commonPrefix(names);
			foreach (qpwgraph_port *port, info.col_ports) {
				Line line;
				line.node_name = node->nodeName();
				line.node_id = node->nodeId();
				line.node_type = node->nodeType();
				line.is_port = true;
				line.port = refOf(port);
				line.port_name = port->portName().mid(prefix.length());
				m_col_lines.append(line);
			}
		}
	}

	if (m_grid)
		m_grid->updateLayout();
}


// Port-type filter inquirer.
bool qpwgraph_matrix::isPortTypeEnabled ( uint port_type ) const
{
	return m_filter_types.value(port_type, true);
}


// Register a filter toggle-action for a port-type, if not already.
void qpwgraph_matrix::addPortTypeFilter (
	uint port_type, const QString& text, bool enabled )
{
	if (m_filter_actions.contains(port_type))
		return;

	QAction *action = new QAction(text, m_filter_toolbar);
	action->setCheckable(true);
	action->setChecked(enabled);
	action->setData(port_type);

	QObject::connect(action,
		SIGNAL(toggled(bool)),
		SLOT(filterActionToggled(bool)));

	m_filter_toolbar->addAction(action);
	m_filter_actions.insert(port_type, action);
	m_filter_types.insert(port_type, enabled);
}


// Widget event handler.
void qpwgraph_matrix::showEvent ( QShowEvent *event )
{
	QWidget::showEvent(event);

	if (m_dirty)
		rebuild();
}


// Address of a (still live) port.
qpwgraph_matrix::PortRef qpwgraph_matrix::refOf ( qpwgraph_port *port ) const
{
	PortRef ref;

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


// Longest common prefix shared by every name in the list.
QString qpwgraph_matrix::commonPrefix ( const QStringList& names )
{
	if (names.count() < 2)
		return QString();

	QString prefix = names.first();
	foreach (const QString& name, names) {
		int len = 0;
		const int max_len = qMin(prefix.length(), name.length());
		while (len < max_len && prefix.at(len) == name.at(len))
			++len;
		prefix.truncate(len);
		if (prefix.isEmpty())
			return prefix;
	}

	// Never strip a name down to nothing (eg. "AUX1" alongside "AUX10").
	foreach (const QString& name, names) {
		if (name.length() <= prefix.length())
			return QString();
	}

	// Keep meaningful port-category words intact even when they land
	// inside the redundant prefix -- never strip through one of these.
	static const char * const kKeepWords[] = {
		"aux", "monitor", "capture", "playback", "input", "output"
	};
	const QString lower = prefix.toLower();
	int cut = prefix.length();
	for (size_t i = 0; i < sizeof(kKeepWords) / sizeof(kKeepWords[0]); ++i) {
		const int pos = lower.indexOf(QLatin1String(kKeepWords[i]));
		if (pos >= 0 && pos < cut)
			cut = pos;
	}
	prefix.truncate(cut);

	return prefix;
}


// Node group collapse-state key/helpers.
quint64 qpwgraph_matrix::nodeKey ( uint node_id, uint node_type )
{
	return (quint64(node_type) << 32) | quint64(node_id);
}


bool qpwgraph_matrix::isRowCollapsed (
	uint node_id, uint node_type, bool auto_collapsed ) const
{
	return m_row_collapsed_user.value(nodeKey(node_id, node_type), auto_collapsed);
}


bool qpwgraph_matrix::isColCollapsed (
	uint node_id, uint node_type, bool auto_collapsed ) const
{
	return m_col_collapsed_user.value(nodeKey(node_id, node_type), auto_collapsed);
}


void qpwgraph_matrix::toggleRowGroup ( uint node_id, uint node_type )
{
	bool collapsed = false;
	foreach (const Line& line, m_row_lines) {
		if (line.group_first
			&& line.node_id == node_id && line.node_type == node_type) {
			collapsed = line.collapsed;
			break;
		}
	}

	m_row_collapsed_user.insert(nodeKey(node_id, node_type), !collapsed);

	rebuild();
}


void qpwgraph_matrix::toggleColGroup ( uint node_id, uint node_type )
{
	bool collapsed = false;
	foreach (const Line& line, m_col_lines) {
		if (line.group_first
			&& line.node_id == node_id && line.node_type == node_type) {
			collapsed = line.collapsed;
			break;
		}
	}

	m_col_collapsed_user.insert(nodeKey(node_id, node_type), !collapsed);

	rebuild();
}


// Cell activation (called by the Grid on a port x port cell click).
void qpwgraph_matrix::activateCell ( int row, int col )
{
	if (row < 0 || row >= m_row_lines.count())
		return;
	if (col < 0 || col >= m_col_lines.count())
		return;

	const Line& rline = m_row_lines.at(row);
	const Line& cline = m_col_lines.at(col);
	if (!rline.is_port || !cline.is_port)
		return;

	// Re-resolve to live ports, as the ones the grid was last built
	// from may since have been destroyed and recreated by the engine's
	// own periodic graph reconciliation, even for a logically unchanged
	// port...
	qpwgraph_port *port1 = resolvePort(rline.port, qpwgraph_item::Output);
	qpwgraph_port *port2 = resolvePort(cline.port, qpwgraph_item::Input);

	if (port1 == nullptr || port2 == nullptr)
		return;
	if (port1->portType() != port2->portType())
		return;

	const bool is_connect = (port1->findConnect(port2) == nullptr);

	// Goes through the very same command/undo-stack, patchbay
	// bookkeeping and (dis)connected notifications the graph canvas
	// itself uses on a drag-connect gesture...
	if (m_canvas)
		m_canvas->connectPorts(port1, port2, is_connect);
}


// Cell paint/hit-test info, resolved fresh (never cached).
qpwgraph_matrix::CellInfo qpwgraph_matrix::cellInfo ( int row, int col ) const
{
	CellInfo info;

	if (row < 0 || row >= m_row_lines.count())
		return info;
	if (col < 0 || col >= m_col_lines.count())
		return info;

	const Line& rline = m_row_lines.at(row);
	const Line& cline = m_col_lines.at(col);
	if (!rline.is_port || !cline.is_port)
		return info;

	qpwgraph_port *port1 = resolvePort(rline.port, qpwgraph_item::Output);
	qpwgraph_port *port2 = resolvePort(cline.port, qpwgraph_item::Input);
	if (port1 == nullptr || port2 == nullptr)
		return info;

	info.valid = true;

	if (port1->portType() != port2->portType()) {
		info.incompatible = true;
		return info;
	}

	if (port1->findConnect(port2)) {
		info.connected = true;
		info.color = m_canvas->portTypeColor(port1->portType());
	}

	return info;
}


// Display text for a row/column line.
QString qpwgraph_matrix::lineLabel ( const Line& line ) const
{
	// Group/folder heading line -- never a real port, whether
	// collapsed or expanded.
	if (line.group_first) {
		return (line.collapsed ? QStringLiteral("▸ ") : QStringLiteral("▾ "))
			+ line.node_name;
	} else {
		return QStringLiteral("     ") + line.port_name;
	}
}


// Hover tooltip text for a grid cell ("input -> output").
QString qpwgraph_matrix::cellTooltip ( int row, int col ) const
{
	if (row < 0 || row >= m_row_lines.count())
		return QString();
	if (col < 0 || col >= m_col_lines.count())
		return QString();

	const Line& rline = m_row_lines.at(row); // output
	const Line& cline = m_col_lines.at(col); // input
	if (!rline.is_port || !cline.is_port)
		return QString();

	return cline.node_name + ':' + cline.port_name
		+ QStringLiteral(" -> ")
		+ rline.node_name + ':' + rline.port_name;
}


// end of qpwgraph_matrix.cpp
