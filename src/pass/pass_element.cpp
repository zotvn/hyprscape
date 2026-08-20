#include "pass_element.hpp"

std::vector<UP<IPassElement>> HSPassElement::draw() {
    return {};
}

bool HSPassElement::needsLiveBlur() {
    return false;
}

bool HSPassElement::needsPrecomputeBlur() {
    return true;
}

bool HSPassElement::disableSimplification() {
    return true;
}
