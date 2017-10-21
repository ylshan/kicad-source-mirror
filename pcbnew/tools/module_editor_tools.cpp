/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2014-2015 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "module_editor_tools.h"
#include "kicad_clipboard.h"
#include "selection_tool.h"
#include "pcb_actions.h"
#include <tool/tool_manager.h>

#include <class_draw_panel_gal.h>
#include <view/view_controls.h>
#include <view/view_group.h>
#include <pcb_painter.h>
#include <origin_viewitem.h>

#include <kicad_plugin.h>
#include <pcbnew_id.h>
#include <collectors.h>
#include <confirm.h>
#include <dialogs/dialog_enum_pads.h>
#include <hotkeys.h>
#include <bitmaps.h>

#include <wxPcbStruct.h>
#include <class_board.h>
#include <class_module.h>
#include <class_edge_mod.h>
#include <board_commit.h>

#include <tools/tool_event_utils.h>

#include <functional>
using namespace std::placeholders;
#include <wx/defs.h>

// Module editor tools
TOOL_ACTION PCB_ACTIONS::placePad( "pcbnew.ModuleEditor.placePad",
        AS_GLOBAL, 0,
        _( "Add Pad" ), _( "Add a pad" ), NULL, AF_ACTIVATE );

TOOL_ACTION PCB_ACTIONS::createPadFromShapes( "pcbnew.ModuleEditor.createPadFromShapes",
        AS_CONTEXT, 0,
        _( "Create Pad from Selected Shapes" ),
        _( "Creates a custom-shaped pads from a set of selected shapes" ),
        primitives_to_custom_pad_xpm );

TOOL_ACTION PCB_ACTIONS::explodePadToShapes( "pcbnew.ModuleEditor.explodePadToShapes",
        AS_CONTEXT, 0,
        _( "Explode Selected Pad to Graphical Shapes" ),
        _( "Converts a custom-shaped pads to a set of graphical shapes" ),
        custom_pad_to_primitives_xpm );

TOOL_ACTION PCB_ACTIONS::enumeratePads( "pcbnew.ModuleEditor.enumeratePads",
        AS_GLOBAL, 0,
        _( "Enumerate Pads" ), _( "Enumerate pads" ), pad_enumerate_xpm, AF_ACTIVATE );

TOOL_ACTION PCB_ACTIONS::moduleEdgeOutlines( "pcbnew.ModuleEditor.graphicOutlines",
        AS_GLOBAL, 0,
        "", "" );

TOOL_ACTION PCB_ACTIONS::moduleTextOutlines( "pcbnew.ModuleEditor.textOutlines",
       AS_GLOBAL, 0,
       "", "" );


MODULE_EDITOR_TOOLS::MODULE_EDITOR_TOOLS() :
    PCB_TOOL( "pcbnew.ModuleEditor" )
{
}


MODULE_EDITOR_TOOLS::~MODULE_EDITOR_TOOLS()
{
}


void MODULE_EDITOR_TOOLS::Reset( RESET_REASON aReason )
{
}



int MODULE_EDITOR_TOOLS::PlacePad( const TOOL_EVENT& aEvent )
{
    struct PAD_PLACER : public INTERACTIVE_PLACER_BASE
    {
        std::unique_ptr<BOARD_ITEM> CreateItem() override
        {
            D_PAD* pad = new D_PAD ( m_board->m_Modules );
            m_frame->Import_Pad_Settings( pad, false );     // use the global settings for pad
            pad->IncrementPadName( true, true );
            return std::unique_ptr<BOARD_ITEM>( pad );
        }
    };

    PAD_PLACER placer;

    frame()->SetToolID( ID_MODEDIT_PAD_TOOL, wxCURSOR_PENCIL, _( "Add pads" ) );

    assert( board()->m_Modules );

    doInteractiveItemPlacement( &placer,  _( "Place pad" ), IPO_REPEAT | IPO_SINGLE_CLICK | IPO_ROTATE | IPO_FLIP | IPO_PROPERTIES );

    frame()->SetNoToolSelected();

    return 0;
}


int MODULE_EDITOR_TOOLS::EnumeratePads( const TOOL_EVENT& aEvent )
{
    if( !board()->m_Modules || !board()->m_Modules->PadsList() )
        return 0;

    DIALOG_ENUM_PADS settingsDlg( frame() );

    if( settingsDlg.ShowModal() != wxID_OK )
        return 0;

    Activate();

    GENERAL_COLLECTOR collector;
    const KICAD_T types[] = { PCB_PAD_T, EOT };

    GENERAL_COLLECTORS_GUIDE guide = frame()->GetCollectorsGuide();
    guide.SetIgnoreMTextsMarkedNoShow( true );
    guide.SetIgnoreMTextsOnBack( true );
    guide.SetIgnoreMTextsOnFront( true );
    guide.SetIgnoreModulesVals( true );
    guide.SetIgnoreModulesRefs( true );

    int padNumber = settingsDlg.GetStartNumber();
    wxString padPrefix = settingsDlg.GetPrefix();

    frame()->DisplayToolMsg( _(
                    "Hold left mouse button and move cursor over pads to enumerate them" ) );

    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );
    getViewControls()->ShowCursor( true );
    frame()->GetGalCanvas()->SetCursor( wxCURSOR_HAND );

    KIGFX::VIEW* view = m_toolMgr->GetView();
    VECTOR2I oldCursorPos;  // store the previous mouse cursor position, during mouse drag
    std::list<D_PAD*> selectedPads;
    BOARD_COMMIT commit( frame() );
    std::map<wxString, wxString> oldNames;
    bool isFirstPoint = true;   // used to be sure oldCursorPos will be initialized at least once.

    while( OPT_TOOL_EVENT evt = Wait() )
    {
        if( evt->IsDrag( BUT_LEFT ) || evt->IsClick( BUT_LEFT ) )
        {
            selectedPads.clear();
            VECTOR2I cursorPos = getViewControls()->GetCursorPosition();

            // Be sure the old cursor mouse position was initialized:
            if( isFirstPoint )
            {
                oldCursorPos = cursorPos;
                isFirstPoint = false;
            }

            // wxWidgets deliver mouse move events not frequently enough, resulting in skipping
            // pads if the user moves cursor too fast. To solve it, create a line that approximates
            // the mouse move and search pads that are on the line.
            int distance = ( cursorPos - oldCursorPos ).EuclideanNorm();
            // Search will be made every 0.1 mm:
            int segments = distance / int( 0.1*IU_PER_MM ) + 1;
            const wxPoint line_step( ( cursorPos - oldCursorPos ) / segments );

            collector.Empty();

            for( int j = 0; j < segments; ++j )
            {
                wxPoint testpoint( cursorPos.x - j * line_step.x,
                                   cursorPos.y - j * line_step.y );
                collector.Collect( board(), types, testpoint, guide );

                for( int i = 0; i < collector.GetCount(); ++i )
                {
                    selectedPads.push_back( static_cast<D_PAD*>( collector[i] ) );
                }
            }

            selectedPads.unique();

            for( D_PAD* pad : selectedPads )
            {
                // If pad was not selected, then enumerate it
                if( !pad->IsSelected() )
                {
                    commit.Modify( pad );

                    // Rename pad and store the old name
                    wxString newName = wxString::Format( wxT( "%s%d" ), padPrefix.c_str(), padNumber++ );
                    oldNames[newName] = pad->GetName();
                    pad->SetName( newName );
                    pad->SetSelected();
                    getView()->Update( pad );
                }

                // ..or restore the old name if it was enumerated and clicked again
                else if( pad->IsSelected() && evt->IsClick( BUT_LEFT ) )
                {
                    auto it = oldNames.find( pad->GetName() );
                    wxASSERT( it != oldNames.end() );

                    if( it != oldNames.end() )
                    {
                        pad->SetName( it->second );
                        oldNames.erase( it );
                    }

                    pad->ClearSelected();
                    getView()->Update( pad );
                }
            }
        }

        else if( ( evt->IsKeyPressed() && evt->KeyCode() == WXK_RETURN ) ||
                   evt->IsDblClick( BUT_LEFT ) )
        {
            commit.Push( _( "Enumerate pads" ) );
            break;
        }

        else if( evt->IsCancel() || evt->IsActivate() )
        {
            commit.Revert();
            break;
        }

        // Prepare the next loop by updating the old cursor mouse position
        // to this last mouse cursor position
        oldCursorPos = getViewControls()->GetCursorPosition();
    }

    for( auto p : board()->m_Modules->Pads() )
    {
        p->ClearSelected();
        view->Update( p );
    }

    frame()->DisplayToolMsg( wxEmptyString );
    frame()->GetGalCanvas()->SetCursor( wxCURSOR_ARROW );

    return 0;
}


int MODULE_EDITOR_TOOLS::ModuleTextOutlines( const TOOL_EVENT& aEvent )
{
    KIGFX::VIEW* view = getView();
    KIGFX::PCB_RENDER_SETTINGS* settings =
            static_cast<KIGFX::PCB_RENDER_SETTINGS*>( view->GetPainter()->GetSettings() );

    const LAYER_NUM layers[] = { LAYER_MOD_TEXT_BK,
                                 LAYER_MOD_TEXT_FR,
                                 LAYER_MOD_TEXT_INVISIBLE,
                                 LAYER_MOD_REFERENCES,
                                 LAYER_MOD_VALUES };

    bool enable = !settings->GetSketchMode( layers[0] );

    for( LAYER_NUM layer : layers )
        settings->SetSketchMode( layer, enable );

    for( auto module : board()->Modules() )
    {
        for( auto item : module->GraphicalItems() )
        {
            if( item->Type() == PCB_MODULE_TEXT_T )
                view->Update( item, KIGFX::GEOMETRY );
        }

        view->Update( &module->Reference(), KIGFX::GEOMETRY );
        view->Update( &module->Value(), KIGFX::GEOMETRY );
    }

    frame()->GetGalCanvas()->Refresh();

    return 0;
}


int MODULE_EDITOR_TOOLS::ModuleEdgeOutlines( const TOOL_EVENT& aEvent )
{
    KIGFX::VIEW* view = getView();
    KIGFX::PCB_RENDER_SETTINGS* settings =
            static_cast<KIGFX::PCB_RENDER_SETTINGS*>( view->GetPainter()->GetSettings() );

    const PCB_LAYER_ID layers[] = { F_Adhes, B_Adhes, F_Paste, B_Paste,
            F_SilkS, B_SilkS, F_Mask, B_Mask,
            Dwgs_User, Cmts_User, Eco1_User, Eco2_User, Edge_Cuts };

    bool enable = !settings->GetSketchMode( layers[0] );

    for( LAYER_NUM layer : layers )
        settings->SetSketchMode( layer, enable );

    for( auto module : board()->Modules() )
    {
        for( auto item : module->GraphicalItems() )
        {
            if( item->Type() == PCB_MODULE_EDGE_T )
                view->Update( item, KIGFX::GEOMETRY );
        }
    }

    frame()->GetGalCanvas()->Refresh();

    return 0;
}

int MODULE_EDITOR_TOOLS::ExplodePadToShapes( const TOOL_EVENT& aEvent )
{
    SELECTION& selection = m_toolMgr->GetTool<SELECTION_TOOL>()->GetSelection();
    BOARD_COMMIT commit( frame() );

    if( selection.Size() != 1 )
        return 0;

    if( selection[0]->Type() != PCB_PAD_T )
        return 0;

    auto pad = static_cast<D_PAD*>( selection[0] );

    if( pad->GetShape() != PAD_SHAPE_CUSTOM )
        return 0;

    commit.Modify( pad );

    wxPoint anchor = pad->GetPosition();

    for( auto prim : pad->GetPrimitives() )
    {
        auto ds = new EDGE_MODULE( board()->m_Modules );

        prim.ExportTo( ds );    // ExportTo exports to a DRAWSEGMENT
        // Fix an arbitray draw layer for this EDGE_MODULE
        ds->SetLayer( Dwgs_User ); //pad->GetLayer() );
        ds->Move( anchor );

        commit.Add( ds );
    }

    pad->SetShape( pad->GetAnchorPadShape() );
    commit.Push( _("Explode pad to shapes") );

    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    return 0;
}


int MODULE_EDITOR_TOOLS::CreatePadFromShapes( const TOOL_EVENT& aEvent )
{
    SELECTION& selection = m_toolMgr->GetTool<SELECTION_TOOL>()->GetSelection();

    std::unique_ptr<D_PAD> pad ( new D_PAD ( board()->m_Modules ) );
    D_PAD *refPad = nullptr;
    bool multipleRefPadsFound = false;
    bool illegalItemsFound = false;

    std::vector<PAD_CS_PRIMITIVE> shapes;

    BOARD_COMMIT commit( frame() );

    for( auto item : selection )
    {
        switch( item->Type() )
        {
            case PCB_PAD_T:
            {
                if( refPad )
                    multipleRefPadsFound = true;

                refPad = static_cast<D_PAD*>( item );
                break;
            }

            case PCB_MODULE_EDGE_T:
            {
                auto em = static_cast<EDGE_MODULE*> ( item );

                PAD_CS_PRIMITIVE shape( em->GetShape() );
                shape.m_Start = em->GetStart();
                shape.m_End = em->GetEnd();
                shape.m_Radius = em->GetRadius();
                shape.m_Thickness = em->GetWidth();
                shape.m_ArcAngle = em->GetAngle();
                shape.m_Poly = em->GetPolyPoints();

                shapes.push_back(shape);

                break;
            }

            default:
            {
                illegalItemsFound = true;
                break;
            }
        }
    }

    if( refPad && selection.Size() == 1 )
    {
        // don't convert a pad into itself...
        return 0;
    }

    if( multipleRefPadsFound )
    {
        DisplayErrorMessage( frame(),
            _(  "Cannot convert items to a custom-shaped pad:\n"
                "selection contains more than one reference pad." ) );
        return 0;
    }

    if( illegalItemsFound )
    {
        DisplayErrorMessage( frame(),
            _( "Cannot convert items to a custom-shaped pad:\n"
               "selection contains unsupported items.\n"
               "Only graphical lines, circles, arcs and polygons are allowed." ) );
        return 0;
    }

    if( refPad )
    {
        pad.reset( static_cast<D_PAD*>( refPad->Clone() ) );
    }
    else
    {
        // Create a default pad anchor:
        pad->SetAnchorPadShape( PAD_SHAPE_CIRCLE );
        pad->SetAttribute( PAD_ATTRIB_SMD );
        pad->SetLayerSet( D_PAD::SMDMask() );
        int radius = Millimeter2iu( 0.2 );
        pad->SetSize ( wxSize( radius, radius ) );
        pad->IncrementPadName( true, true );
    }


    pad->SetPrimitives( shapes );
    pad->SetShape ( PAD_SHAPE_CUSTOM );

    boost::optional<VECTOR2I> anchor;
    VECTOR2I tmp;

    if( refPad )
    {
        anchor = pad->GetPosition();
    }
    else if( pad->GetBestAnchorPosition( tmp ) )
    {
        anchor = tmp;
    }

    if( !anchor )
    {
        DisplayErrorMessage( frame(),
            _( "Cannot convert items to a custom-shaped pad:\n"
               "unable to determine the anchor point position.\n"
               "Consider adding a small anchor pad to the selection and try again.") );
        return 0;
    }


    // relocate the shapes, they are relative to the anchor pad position
    for( auto& shape : shapes )
    {
        shape.Move( wxPoint( -anchor->x, -anchor->y ) );
    }


    pad->SetPosition( wxPoint( anchor->x, anchor->y ) );
    pad->SetPrimitives( shapes );

    bool result = pad->MergePrimitivesAsPolygon();

    if( !result )
    {
        DisplayErrorMessage( frame(),
                _( "Cannot convert items to a custom-shaped pad:\n"
                   "selected items do not form a single solid shape.") );
        return 0;
    }

    auto padPtr = pad.release();

    commit.Add( padPtr );
    for ( auto item : selection )
    {
        commit.Remove( item );
    }

    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );
    m_toolMgr->RunAction( PCB_ACTIONS::selectItem, true, padPtr );

    commit.Push(_("Create Pad from Selected Shapes") );

    return 0;
}

void MODULE_EDITOR_TOOLS::setTransitions()
{
    Go( &MODULE_EDITOR_TOOLS::PlacePad,            PCB_ACTIONS::placePad.MakeEvent() );
    Go( &MODULE_EDITOR_TOOLS::CreatePadFromShapes, PCB_ACTIONS::createPadFromShapes.MakeEvent() );
    Go( &MODULE_EDITOR_TOOLS::ExplodePadToShapes,  PCB_ACTIONS::explodePadToShapes.MakeEvent() );
    Go( &MODULE_EDITOR_TOOLS::EnumeratePads,       PCB_ACTIONS::enumeratePads.MakeEvent() );
    Go( &MODULE_EDITOR_TOOLS::ModuleTextOutlines,  PCB_ACTIONS::moduleTextOutlines.MakeEvent() );
    Go( &MODULE_EDITOR_TOOLS::ModuleEdgeOutlines,  PCB_ACTIONS::moduleEdgeOutlines.MakeEvent() );
}
