#include <vector>
#include <map>
#include <shared_mutex>
#include <chrono>

// Testing
#include <iostream> // cout
#include <unistd.h> // sleep

// Compile with:
// g++ --std=c++17 rplace.cpp -o rplace

// Forward declarations cause everything's in one file.
class Place;

// This is a "display pixel" it does not contain the information required to recreate the grid from scratch, just to
// display it onscreen. A list of display pixels could be applied to a visual representation of a Place to update it's
// state.
class Pixel {
  private:
    uint64_t x;
    uint64_t y;
    uint64_t color;
    uint64_t userID;

  public:
    Pixel(uint64_t x, uint64_t y, uint64_t color, uint64_t userID) :
        x(x),
        y(y),
        color(color),
        userID(userID)
    {
    }

    uint64_t getX() const {return x;}
    uint64_t getY() const {return y;}
    uint64_t getColor() const {return color;}
    uint64_t getUserID() const {return userID;}

    // Probably black, current /r/place uses white. Could change with actual colors.
    static constexpr uint64_t defaultColor = 0;
};

// What's the schema for a change look like?
// This is not designed to be space optimized, it's designed to be forward compatible if we scale up the grid in the
// future or expand the color palette. If this can't scale, we can potentially shrink it.
// With 6 8-byte numbers, we have a total space requirement of 48 bytes per update.
class Update {
  public:
    const uint64_t recordNumber;
    const uint64_t timestamp;
    const Pixel pixel;

    Update(uint64_t recordNumber, uint64_t timestamp, const Pixel& pixel) :
        recordNumber(recordNumber),
        timestamp(timestamp),
        pixel(pixel)
    {
    }

    // Enable using emplace in containers.
    Update(const Update&& other) :
        recordNumber(other.recordNumber),
       timestamp(other.timestamp),
       pixel(other.pixel) 
    {
    }
};

// A snapshot is the current state of the Place after a particular number of changes.
class Snapshot {
  public:
    const uint64_t width;
    const uint64_t height;

    // The entire set of pixels requires to make up a Place.
    std::vector<Pixel> pixels;
    
    // This is the count in the update stream for this snapshot.
    uint64_t recordNumber;

    // Default constructor.
    Snapshot(uint64_t width, uint64_t height);

    // Apply a set of updates to a Snapshot. This takes everything from `recordNumber` (inclusive) forward and applies
    // it to this Snapshot.
    void apply(const std::vector<const Update>& updates);
};

Snapshot::Snapshot(uint64_t width, uint64_t height) :
    width(width),
    height(height),
    recordNumber(0)
{
    // Allocate a bunch of data.
    pixels.reserve(width * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels.emplace_back(x, y, 0, Pixel::defaultColor);
        }
    }
}

void Snapshot::apply(const std::vector<const Update>& updates) {
    // TODO: Minor optimization, I think this reapplies the first item in this set even though it's already applied.
    for (size_t i = recordNumber; i < updates.size(); i++) {
        pixels[updates[i].pixel.getY() * width + updates[i].pixel.getX()] = updates[i].pixel;
    }
    recordNumber = updates.size();
}

class Place {
  public:

    // This gets the current representation of the Place.
    Snapshot getCurrentState();

    // Applies the update specified in the given pixel. Returns true if success, false if failure.
    // Failure cases can be:
    // 1. The pixel doesn't fit in the Place.
    // 2. The user has written to the Place too recently.
    bool update(const Pixel& p);

    // TODO:
    // void save(); write to a file. Can work in batches appending chunks of updates.
    // void load(); opposite of serialize. Load from a saved file.
    // std::vector<const Update> getDiff(size_t fromUpdateNumber); // Returns the difference since a particular update.

    // The dimensions are fixed, though you could create a new Place that expands or contracts from a previous Place.
    const uint64_t width = 1000;
    const uint64_t height = 1000;

    // Default constructor.
    Place();

  private:
    // Map from userIDs to timestamps (unix epoch in us).
    std::map<uint64_t, uint64_t> mostRecentUpdatesPerUser;

    // List of all updates from the beginning of time.
    std::vector<const Update> updates;

    Snapshot workingSnapshot;
    std::shared_ptr<const Snapshot> recentSnapshot;

    // Mutex for locking around updates.
    std::shared_mutex updateMutex;
};

Place::Place() :
    workingSnapshot(width, height),
    recentSnapshot(std::make_shared<Snapshot>(width, height))
{
}

Snapshot Place::getCurrentState() {
    std::shared_ptr<const Snapshot> recentCopy;
    {
        // We lock to update the main snapshot state. Generally this is fast as we do this all the time, so there
        // shouldn't be a lot of updates to apply.
        std::unique_lock<std::shared_mutex> lock(updateMutex);

        // Update the working snapshot to the latest (fast).
        workingSnapshot.apply(updates);

        // If we're significantly ahead, replace the "recent" snapshot. This involves copying the entire 32mb object,
        // so it will be relatively slow, so we don't do it that often.
        if (workingSnapshot.recordNumber > recentSnapshot->recordNumber + 100) {
            // This invokes the copy constructor, meaning recentSnapshot is now a *new* object, but any existing shared_ptr's
            // are still pointing at the old object.
            recentSnapshot = std::make_shared<const Snapshot>(workingSnapshot);
        }

        // This will be some value from within the last 100 updates.
        recentCopy = recentSnapshot;
    }

    // We now copy the value of `recentSnapshot` outside of the main mutex lock. This could be the second copy of this
    // object if we updated `recentSnapshot` above, but importantly we don't need to hold the lock to do it.
    Snapshot returnValue = *recentCopy;

    // We can now apply the recent changes to the copy with a read-only lock, as we are only modifying our return
    // object, not the Place object itself. This should "also" be fast-ish, it can apply up to 100 updates.
    {
        std::shared_lock<std::shared_mutex> lock(updateMutex);
        returnValue.apply(updates);
    }

    return returnValue;
}

bool Place::update(const Pixel& p) {

    // Fail early if this isn't a valid location.
    if (p.getX() >= width || p.getY() >= height) {
        return false;
    }

    // Lock to prevent collisions.
    std::unique_lock<std::shared_mutex> lock(updateMutex);
    
    // Don't grab the current time until we're locked, in case it takes a while.
    auto currentTime = std::chrono::system_clock::now().time_since_epoch().count();

    // See if this user has ever updated the Place.
    auto recentUdpateIt = mostRecentUpdatesPerUser.find(p.getUserID());
    if (recentUdpateIt != mostRecentUpdatesPerUser.end()) {
        // This user *has* updated the grid at some point.
        if (recentUdpateIt->second > currentTime - (/*60 * */5 * 1'000'000)) {
            // Less than 5 minutes. // TODO: Actually seconds, update later!
            return false;
        } else {
            // More than 5 minutes, we can update.
            recentUdpateIt->second = currentTime;
        }
    } else {
        // If the user had never update the Place, we'll add them to the map.
        mostRecentUpdatesPerUser.emplace(std::make_pair(p.getUserID(), currentTime));
    }

    // Now, if we get this far, we add an update to the complete list (regardless if the user had previously updated
    // the Place or not).
    updates.emplace_back(updates.size(), currentTime, p);

    // Done, success.
    return true;
}

// Main is not really the right place to call this, but it's all conceptual so far.
int main() {
    Place place;

    // Trivial testing.
    bool result = place.update(Pixel{0,0,0,0});
    if (result) {
        sleep(3);
        result = place.update(Pixel{0,0,0,0});
        if (result) {
            std::cout << "Failed!" << std::endl;
        } else {
            std::cout << "OK!" << std::endl;
            sleep(3);
            result = place.update(Pixel{0,0,0,0});
            if (result) {
                std::cout << "Still ok!" << std::endl;
            }
        }
    }

}
