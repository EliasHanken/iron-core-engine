#pragma once

// The start and pause menus, as plain rules: what's selected, what a key does, and
// what the player chose. Drawing lives in main.cpp, so this runs without a GPU.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace kindle {

struct Settings {
    float sound = 0.8f;
    float look = 0.5f;  // 0.5 is the normal turning speed.

    void load(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "r");
        if (!file) return;
        float a = sound;
        float b = look;
        if (std::fscanf(file, "%f %f", &a, &b) == 2) {
            sound = std::clamp(a, 0.0f, 1.0f);
            look = std::clamp(b, 0.0f, 1.0f);
        }
        std::fclose(file);
    }

    void save(const std::string& path) const {
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (!file) return;
        std::fprintf(file, "%.3f %.3f\n", sound, look);
        std::fclose(file);
    }
};

enum class Choice { None, Begin, Resume, Restart, Quit };

class Menu {
public:
    struct Item {
        std::string label;
        Choice choice = Choice::None;
        float* slider = nullptr;  // Set for a value the player can adjust.
    };

    static constexpr float kSliderStep = 0.05f;

    void show(std::vector<Item> items) {
        items_ = std::move(items);
        selected_ = 0;
    }

    const std::vector<Item>& items() const { return items_; }
    std::size_t selected() const { return selected_; }

    // Moves the selection, wrapping at the ends. Returns whether it moved.
    bool move(int delta) {
        if (items_.size() < 2 || delta == 0) return false;
        const int count = static_cast<int>(items_.size());
        selected_ = static_cast<std::size_t>((static_cast<int>(selected_) + delta % count + count) % count);
        return true;
    }

    bool select(std::size_t index) {
        if (index >= items_.size() || index == selected_) return false;
        selected_ = index;
        return true;
    }

    // Steps the selected slider. Returns whether anything changed.
    bool adjust(int delta) {
        return !items_.empty() && setSlider(selected_, *sliderOf(selected_) + kSliderStep * static_cast<float>(delta));
    }

    bool setSlider(std::size_t index, float value) {
        float* slider = sliderOf(index);
        if (slider == &none_) return false;
        const float next = std::clamp(value, 0.0f, 1.0f);
        if (next == *slider) return false;
        *slider = next;
        return true;
    }

    // Enter or a click on the selected item.
    Choice activate() const { return items_.empty() ? Choice::None : items_[selected_].choice; }

private:
    float* sliderOf(std::size_t index) {
        return index < items_.size() && items_[index].slider ? items_[index].slider : &none_;
    }

    std::vector<Item> items_;
    std::size_t selected_ = 0;
    float none_ = 0.0f;
};

}  // namespace kindle
