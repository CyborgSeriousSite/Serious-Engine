/* Copyright (c) 2021 SeriousAlexej (Oleksii Sierov).
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation


This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */
#ifndef PROPERTIES_ENTITY_ROOT_H
#define PROPERTIES_ENTITY_ROOT_H

#include "base_property_tree_item.h"

#include <vector>

class EntityRootProperties : public BasePropertyTreeItem
{
public:
  EntityRootProperties(BasePropertyTreeItem* parent, const std::vector<CEntity*>& entities);

  QVariant data(int column, int role) const override;

private:
  const std::vector<CEntity*> m_entities;
};

#endif
