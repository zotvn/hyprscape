#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>

// A no-op pass element whose only job is to answer `true` to disableSimplification(). Without
// it the render pass may decide an opaque element fully covers the ones behind it and drop
// them -- which is wrong once every element is being moved by a renderModif.
class HSPassElement : public IPassElement {
  public:
    HSPassElement() = default;
    virtual ~HSPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool needsLiveBlur() override;
    virtual bool needsPrecomputeBlur() override;
    virtual bool disableSimplification() override;

    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }

    virtual const char* passName() override {
        return "HSDisableSimplification";
    }
};
