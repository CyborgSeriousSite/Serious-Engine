/* Copyright (c) 2002-2012 Croteam Ltd.
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

#include "StdH.h"
#include <Engine/CurrentVersion.h>
#include "MenuPrinting.h"
#include "VarList.h"
#include "MVar.h"

// [Cecil] Extra functionality
#include "Cecil/CecilExtensions.h"

// [Cecil] For tab switching
#include "GUI/Menus/MenuManager.h"
extern CMenuGadget *_pmgLastActivatedGadget;
extern CMenuGadget *_pmgUnderCursor;

extern BOOL _bVarChanged;

// [Cecil] Switch to another options tab
void SelectOptionsTab(void) {
  // Tabs are disabled
  if (!sam_bOptionTabs) return;

  CMGButton &mgTab = (CMGButton &)*_pmgLastActivatedGadget;
  CVarMenu &gm = _pGUIM->gmVarMenu;

  // Same tab
  if (gm.gm_iTab == mgTab.mg_iIndex) {
    return;
  }

  _pmgUnderCursor = NULL;

  // Select a new tab
  gm.gm_iTab = mgTab.mg_iIndex;

  // Disable current tab button
  for (INDEX iTab = 0; iTab < gm.gm_agmTabs.Count(); iTab++) {
    CMGButton &mgTab = gm.gm_agmTabs[iTab];
    mgTab.mg_bEnabled = (iTab != gm.gm_iTab);
  }

  // Reload menu
  gm.gm_iListOffset = 0;
  gm.gm_ctListTotal = _aTabs[gm.gm_iTab].lhVars.Count();
  gm.gm_iListWantedItem = 0;
  gm.CGameMenu::StartMenu();
};

void CVarMenu::Initialize_t(void) {
  gm_mgTitle.mg_boxOnScreen = BoxTitle();
  gm_mgTitle.SetName("");
  AddChild(&gm_mgTitle);

  for (INDEX iLabel = 0; iLabel < VARS_ON_SCREEN; iLabel++) {
    INDEX iPrev = (VARS_ON_SCREEN + iLabel - 1) % VARS_ON_SCREEN;
    INDEX iNext = (iLabel + 1) % VARS_ON_SCREEN;
    // initialize label gadgets
    gm_mgVar[iLabel].mg_pmgUp = &gm_mgVar[iPrev];
    gm_mgVar[iLabel].mg_pmgDown = &gm_mgVar[iNext];
    gm_mgVar[iLabel].mg_pmgLeft = &gm_mgApply;
    gm_mgVar[iLabel].mg_boxOnScreen = BoxMediumRow(iLabel - 1.0f);
    gm_mgVar[iLabel].mg_pActivatedFunction = NULL; // never called!
    AddChild(&gm_mgVar[iLabel]);
  }

  gm_mgApply.mg_boxOnScreen = BoxMediumRow(16.5f);
  gm_mgApply.mg_bfsFontSize = BFS_LARGE;
  gm_mgApply.mg_iCenterI = 1;
  gm_mgApply.mg_pmgLeft =
    gm_mgApply.mg_pmgRight =
    gm_mgApply.mg_pmgUp =
    gm_mgApply.mg_pmgDown = &gm_mgVar[0];
  gm_mgApply.SetText(TRANS("APPLY"));
  gm_mgApply.mg_strTip = TRANS("apply changes");
  AddChild(&gm_mgApply);
  gm_mgApply.mg_pActivatedFunction = NULL;

  AddChild(&gm_mgArrowUp);
  AddChild(&gm_mgArrowDn);
  gm_mgArrowUp.mg_adDirection = AD_UP;
  gm_mgArrowDn.mg_adDirection = AD_DOWN;
  gm_mgArrowUp.mg_boxOnScreen = BoxArrow(AD_UP);
  gm_mgArrowDn.mg_boxOnScreen = BoxArrow(AD_DOWN);
  gm_mgArrowUp.mg_pmgRight = gm_mgArrowUp.mg_pmgDown = &gm_mgVar[0];
  gm_mgArrowDn.mg_pmgRight = gm_mgArrowDn.mg_pmgUp = &gm_mgVar[VARS_ON_SCREEN - 1];

  gm_ctListVisible = VARS_ON_SCREEN;
  gm_pmgArrowUp = &gm_mgArrowUp;
  gm_pmgArrowDn = &gm_mgArrowDn;
  gm_pmgListTop = &gm_mgVar[0];
  gm_pmgListBottom = &gm_mgVar[VARS_ON_SCREEN - 1];

  // [Cecil] First tab
  gm_iTab = 0;
}

void CVarMenu::FillListItems(void) {
  // disable all items first
  for (INDEX i = 0; i < VARS_ON_SCREEN; i++) {
    gm_mgVar[i].mg_bEnabled = FALSE;
    gm_mgVar[i].mg_pvsVar = NULL;
    gm_mgVar[i].mg_iInList = -2;
  }

  // [Cecil] Current tab
  CListHead &lhTab = _aTabs[gm_iTab].lhVars;

  BOOL bHasFirst = FALSE;
  BOOL bHasLast = FALSE;
  INDEX ctLabels = lhTab.Count();
  INDEX iLabel = 0;

  FOREACHINLIST(CVarSetting, vs_lnNode, lhTab, itvs) {
    CVarSetting &vs = *itvs;
    INDEX iInMenu = iLabel - gm_iListOffset;
    if ((iLabel >= gm_iListOffset) && (iLabel < (gm_iListOffset + VARS_ON_SCREEN))) {
      bHasFirst |= (iLabel == 0);
      bHasLast |= (iLabel == ctLabels - 1);
      gm_mgVar[iInMenu].mg_pvsVar = &vs;
      gm_mgVar[iInMenu].mg_strTip = vs.vs_strTip;
      gm_mgVar[iInMenu].mg_bEnabled = gm_mgVar[iInMenu].IsEnabled();
      gm_mgVar[iInMenu].mg_iInList = iLabel;
    }
    iLabel++;
  }
  // enable/disable up/down arrows
  gm_mgArrowUp.mg_bEnabled = !bHasFirst && ctLabels > 0;
  gm_mgArrowDn.mg_bEnabled = !bHasLast && ctLabels > 0;
}

void CVarMenu::StartMenu(void) {
  LoadVarSettings(gm_fnmMenuCFG);

  // [Cecil] Add tab buttons
  if (sam_bOptionTabs) {
    for (INDEX iTab = 0; iTab < _aTabs.Count(); iTab++) {
      const CVarTab &tab = _aTabs[iTab];

      CMGButton &mgTab = gm_agmTabs.Push();
      mgTab.mg_iIndex = iTab;
      mgTab.mg_bfsFontSize = BFS_MEDIUM;
      mgTab.mg_iCenterI = -1;

      mgTab.mg_bEnabled = (iTab != 0);
      mgTab.mg_boxOnScreen = BoxLeftColumn(iTab + 2.0f);
      mgTab.mg_pActivatedFunction = &SelectOptionsTab;

      // Connect previous button to the current one
      if (iTab > 0) {
        mgTab.mg_pmgUp = &gm_agmTabs[iTab - 1];
        gm_agmTabs[iTab - 1].mg_pmgDown = &mgTab;
      }

      mgTab.SetText(tab.strName);

      AddChild(&mgTab);
    }
  }

  // [Cecil] Reset current tab
  gm_iTab = 0;

  // set default parameters for the list
  gm_iListOffset = 0;
  gm_ctListTotal = _aTabs[gm_iTab].lhVars.Count();
  gm_iListWantedItem = 0;
  CGameMenu::StartMenu();
}

void CVarMenu::EndMenu(void) {
  // disable all items first
  for (INDEX i = 0; i < VARS_ON_SCREEN; i++) {
    gm_mgVar[i].mg_bEnabled = FALSE;
    gm_mgVar[i].mg_pvsVar = NULL;
    gm_mgVar[i].mg_iInList = -2;
  }

  // [Cecil] Remove all tabs
  gm_agmTabs.Clear();

  FlushVarSettings(FALSE);
  _bVarChanged = FALSE;
}

void CVarMenu::Think(void) {
  gm_mgApply.mg_bEnabled = _bVarChanged;
  extern void FixupBackButton(CGameMenu * pgm);
  FixupBackButton(this);
}