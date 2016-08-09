/*
 * Copyright (C) 2016 Olzhas Rakhimov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "event.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QStyleOptionGraphicsItem>

namespace scram {
namespace gui {

Event::Event(QGraphicsView */*view*/) {}

namespace {

/**
 * @return Unit width (x) and height (y) for shapes.
 */
QSize units()
{
    QFontMetrics font = QApplication::fontMetrics();
    return {font.averageCharWidth(), font.height()};
}

} // namespace

QRectF Event::boundingRect() const
{
    int w = units().width();
    int h = units().height();
    return QRectF(-8 * w, 0, 16 * w, 5.5 * h);
}

void Event::paint(QPainter *painter, const QStyleOptionGraphicsItem */*option*/,
                  QWidget */*widget*/)
{
    int w = units().width();
    int h = units().height();
    QRectF rect(-8 * w, 0, 16 * w, 3 * h);
    painter->drawRect(rect);
    painter->drawText(rect, Qt::AlignCenter | Qt::TextWordWrap, m_description);

    painter->drawLine(QPointF(0, 3 * h), QPointF(0, 4 * h));

    QRectF nameRect(-5 * w, 4 *h, 10 * w, h);
    painter->drawRect(nameRect);
    painter->drawText(nameRect, Qt::AlignCenter, m_name);

    painter->drawLine(QPointF(0, 5 * h), QPointF(0, 5.5 * h));
}

QRectF BasicEvent::boundingRect() const
{
    QRectF eventRect = Event::boundingRect();
    eventRect.setHeight(eventRect.height() + 10 * units().width());
    return eventRect;
}

void BasicEvent::paint(QPainter *painter,
                       const QStyleOptionGraphicsItem *option,
                       QWidget *widget)
{
    Event::paint(painter, option, widget);
    double r = 5 * units().width();
    painter->drawEllipse(QPointF(0, Event::boundingRect().bottom() + r), r, r);
}

QRectF IntermediateEvent::boundingRect() const
{
    QRectF eventRect = Event::boundingRect();
    eventRect.setHeight(eventRect.height() + 10 * units().width());
    return eventRect;
}

void IntermediateEvent::paint(QPainter *painter,
                              const QStyleOptionGraphicsItem *option,
                              QWidget *widget)
{
    Event::paint(painter, option, widget);
    m_gate->paint(painter, option);
}

} // namespace gui
} // namespace scram
