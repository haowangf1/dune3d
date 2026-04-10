#include "tool_select_revolve_axis.hpp"
#include "document/document.hpp"
#include "document/entity/entity.hpp"
#include "document/group/group_revolve.hpp"
#include "editor/editor_interface.hpp"
#include "tool_common_impl.hpp"
#include "util/action_label.hpp"

namespace dune3d {


ToolBase::CanBegin ToolSelectRevolveAxis::can_begin()
{
    auto &group = get_doc().get_group(m_core.get_current_group());
    return dynamic_cast<GroupRevolve *>(&group);
}

ToolResponse ToolSelectRevolveAxis::begin(const ToolArgs &args)
{
    m_group = &get_doc().get_group<GroupRevolve>(m_core.get_current_group());
    m_selection.clear();
    if (m_group->m_normal) {
        m_selection.insert(SelectableRef{SelectableRef::Type::ENTITY, m_group->m_normal, 0});
    }

    m_intf.enable_hover_selection();
    std::vector<ActionLabelInfo> actions;
    actions.emplace_back(InToolActionID::LMB, "select axis");
    actions.emplace_back(InToolActionID::CANCEL, "cancel");
    m_intf.tool_bar_set_actions(actions);
    m_intf.tool_bar_set_tool_tip("Select axis entity (workplane or line)");

    m_intf.set_no_canvas_update(true);
    m_intf.canvas_update_from_tool();

    return ToolResponse();
}

ToolResponse ToolSelectRevolveAxis::update(const ToolArgs &args)
{
    auto is_valid_axis_sel = [this](const SelectableRef &sel) {
        if (sel.type != SelectableRef::Type::ENTITY)
            return false;
        if (sel.point != 0)
            return false;

        const auto &axis = get_doc().get_entity(sel.item);
        return axis.of_type(Entity::Type::WORKPLANE, Entity::Type::LINE_2D, Entity::Type::LINE_3D);
    };

    if (args.type == ToolEventType::MOVE) {
        if (auto hsel = m_intf.get_hover_selection(); hsel && is_valid_axis_sel(*hsel))
            m_intf.tool_bar_set_tool_tip("Click to select revolve axis");
        else
            m_intf.tool_bar_set_tool_tip("Select axis entity (workplane or line)");

        return ToolResponse();
    }

    if (args.type != ToolEventType::ACTION)
        return ToolResponse();

    switch (args.action) {
    case InToolActionID::LMB: {
        auto hsel = m_intf.get_hover_selection();
        if (!hsel) {
            m_intf.tool_bar_flash("Select an axis entity");
            return ToolResponse();
        }

        if (hsel->type != SelectableRef::Type::ENTITY) {
            m_intf.tool_bar_flash("Select an axis entity");
            return ToolResponse();
        }

        if (hsel->point != 0) {
            m_intf.tool_bar_flash("Select the body of the axis entity");
            return ToolResponse();
        }

        const auto &axis = get_doc().get_entity(hsel->item);
        if (!axis.of_type(Entity::Type::WORKPLANE, Entity::Type::LINE_2D, Entity::Type::LINE_3D)) {
            m_intf.tool_bar_flash("Axis entity must be a line or a workplane");
            return ToolResponse();
        }

        m_group->m_origin = {hsel->item, 1};
        m_group->m_normal = hsel->item;
        set_current_group_generate_pending();
        m_selection = {SelectableRef{SelectableRef::Type::ENTITY, hsel->item, 0}};
        return ToolResponse::commit();
    }
    case InToolActionID::CANCEL:
        m_selection.clear();
        return ToolResponse::revert();

    default:
        return ToolResponse();
    }
}

} // namespace dune3d
