/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FrameGraph.h"

#include <utils/Panic.h>
#include <utils/Log.h>

namespace filament {

FrameGraphPassBase::FrameGraphPassBase(const char* name) noexcept
        : mName(name) {
}

FrameGraphPassBase::~FrameGraphPassBase() = default;

void FrameGraphPassBase::read(FrameGraphResource const& resource) {
    mReads.push_back(resource.mId);
}

void FrameGraphPassBase::write(FrameGraphResource const& resource) {
    mRefCount++;
    resource.mWriters.push_back(this);
}

// ------------------------------------------------------------------------------------------------

FrameGraphBuilder::FrameGraphBuilder(FrameGraph& fg, FrameGraphPassBase* pass) noexcept
    : mFrameGraph(fg), mPass(pass) {
}

FrameGraphResource* FrameGraphBuilder::getResource(FrameGraphResourceHandle input) {
    FrameGraph& frameGraph = mFrameGraph;
    auto const& resourcesIds = frameGraph.mResourcesIds;
    std::vector<FrameGraphResource>& registry = frameGraph.mResourceRegistry;

    if (!ASSERT_POSTCONDITION_NON_FATAL(input.isValid(),
            "attempting to use invalid resource handle=%u", input.getHandle())) {
        return nullptr;
    }

    uint32_t handle = input.getHandle();
    assert(handle < resourcesIds.size());
    FrameGraph::ResourceID id = resourcesIds[handle];

    assert(id.index < registry.size());
    auto& resource = registry[id.index];

    if (!ASSERT_POSTCONDITION_NON_FATAL(id.version == resource.getVersion(),
            "attempting to use invalid ResourceID={%u, %u}, version=",
            id.index, id.version, resource.getVersion())) {
        return nullptr;
    }

    return &resource;
}

FrameGraphResourceHandle FrameGraphBuilder::createTexture(
        const char* name, FrameGraphResource::TextureDesc const& desc) noexcept {
    FrameGraph& frameGraph = mFrameGraph;
    FrameGraphResource& resource = frameGraph.createResource(name);
    mPass->write(resource);
    return frameGraph.createHandle(resource);
}

FrameGraphResourceHandle FrameGraphBuilder::read(FrameGraphResourceHandle const& input) {
    FrameGraphResource* resource = getResource(input);
    if (!resource) {
        return {};
    }
    mPass->read(*resource);
    return input;
}

FrameGraphResourceHandle FrameGraphBuilder::write(FrameGraphResourceHandle const& output) {
    FrameGraphResource* resource = getResource(output);
    if (!resource) {
        return {};
    }
    mPass->write(*resource);

    /*
     * We invalidate and rename handles that are writen into, to avoid undefined order
     * access to the resources.
     *
     * e.g. forbidden graphs
     *
     *         +-> [R1] -+
     *        /           \
     *  (A) -+             +-> (A)
     *        \           /
     *         +-> [R2] -+        // failure when setting R2 from (A)
     *
     */

    // rename handle
    FrameGraph& frameGraph = mFrameGraph;
    resource->mId.version++; // this invalidates previous handles
    return frameGraph.createHandle(*resource);
}

// ------------------------------------------------------------------------------------------------

FrameGraph::FrameGraph() = default;

FrameGraph::~FrameGraph() = default;

bool FrameGraph::isValid(FrameGraphResourceHandle handle) const noexcept {
    if (!handle.isValid()) return false;

    auto const& resourcesIds = mResourcesIds;
    assert(handle.getHandle() < resourcesIds.size());
    FrameGraph::ResourceID id = resourcesIds[handle.getHandle()];

    auto const& registry = mResourceRegistry;
    assert(id.index < registry.size());
    auto& resource = registry[id.index];

    return id.version == resource.getVersion();
}

void FrameGraph::present(FrameGraphResourceHandle input) {
    struct Dummy {
    };
    addPass<Dummy>("Present",
            [&input](FrameGraphBuilder& builder, Dummy& data) {
                builder.read(input);
            },
            [](FrameGraphPassResources const& resources, Dummy const& data) {
            });
}

FrameGraphResource& FrameGraph::createResource(const char* name) {
    auto& registry = mResourceRegistry;
    uint32_t id = (uint32_t)registry.size();
    registry.emplace_back(name, id);
    return registry.back();
}

FrameGraphResourceHandle FrameGraph::createHandle(FrameGraphResource const& resource) {
    FrameGraphResourceHandle handle{ (uint32_t)mResourcesIds.size() };
    mResourcesIds.push_back(resource.mId);
    return handle;
}

FrameGraph& FrameGraph::compile() noexcept {
    auto& registry = mResourceRegistry;
    for (auto& pass : mFrameGraphPasses) {
        auto const& reads = pass->getReadResources();
        for (auto id : reads) {
            FrameGraphResource& resource = registry[id.index];

            // add a reference for each pass that reads from this resource
            resource.mRefCount++;

            // figure out which is the first pass to need this resource
            resource.mFirst = resource.mFirst ? resource.mFirst : pass.get();

            // figure out which is the last pass to need this resource
            resource.mLast = pass.get();
        }
    }

    std::vector<FrameGraphResource*> stack;
    stack.reserve(registry.size());
    for (FrameGraphResource& resource : registry) {
        if (resource.mRefCount == 0) {
            stack.push_back(&resource);
        }
    }

    while (!stack.empty()) {
        FrameGraphResource* resource = stack.back();
        stack.pop_back();

        // by construction, this resource cannot have more than one producer because
        // - two unrelated passes can't write in the same resource
        // - passes that read + write into the resource imply that the refcount is not null

        assert(resource->mWriters.size() == 1);

        FrameGraphPassBase* const writer = resource->mWriters.back();

        assert(writer->mRefCount >= 1);

        if (--writer->mRefCount == 0) {
            // this pass is culled
            auto const& reads = writer->getReadResources();
            for (auto id : reads) {
                resource = &registry[id.index];
                resource->mRefCount--;
                if (resource->mRefCount == 0) {
                    stack.push_back(resource);
                }
            }
        }
    }

    for (FrameGraphResource& resource : registry) {
        FrameGraphPassBase* const writer = resource.mWriters.back();
        if (writer->mRefCount) {
            if (resource.mRefCount == 0) {
                // we have a resource with a ref-count of zero, written to by an active (non-culled)
                // pass. Is this allowable? This would happen for instance if a pass were to write
                // in two buffers, but only one of them when consumed. The pass couldn't be
                // discarded, but the unused resource would have to be created.
                // TODO: should we allow this?
            }

            assert(resource.mFirst);
            assert(resource.mLast);

            resource.mFirst->mDevirtualize.push_back(resource.getId());
            resource.mLast->mDestroy.push_back(resource.getId());
        }
    }

    return *this;
}

void FrameGraph::execute() noexcept {
    auto const& resources = mResources;
    auto& registry = mResourceRegistry;
    for (auto& pass : mFrameGraphPasses) {
        if (!pass->mRefCount) continue;

        for (size_t id : pass->mDevirtualize) {
            // TODO: create concrete resources
            FrameGraphResource& resource = registry[id];
        }

        pass->execute(resources);

        for (uint32_t id : pass->mDestroy) {
            // TODO: delete concrete resources
            FrameGraphResource& resource = registry[id];
        }
    }

    // reset the frame graph state
    mFrameGraphPasses.clear();
    mResourcesIds.clear();
    mResourceRegistry.clear();
}

void FrameGraph::export_graphviz(utils::io::ostream& out) {
    bool removeCulled = false;

    out << "digraph framegraph {\n";
    out << "rankdir = LR\n";
    out << "bgcolor = black\n";
    out << "node [shape=rectangle, fontname=\"helvetica\", fontsize=10]\n\n";

    auto const& resourcesIds = mResourcesIds;
    auto const& registry = mResourceRegistry;

    for (auto const& pass : mFrameGraphPasses) {
        if (removeCulled && !pass->mRefCount) continue;
        out << "\"" << pass->getName() << "\" [label=\"" << pass->getName()
               << "\\nrefs: " << pass->mRefCount
               << "\", style=filled, fillcolor="
               << (pass->mRefCount ? "darkorange" : "darkorange4") << "]\n";
    }

    out << "\n";
    for (auto const& id : resourcesIds) {
        auto const& resource = registry[id.index];
        if (removeCulled && !resource.mRefCount) continue;
        out << "\"" << resource.getName() << "\\n(version: " << id.version << ")\""
               "[label=\"" << resource.getName() << "\\n(version: " << id.version << ")"
               "\\nid:" << id.index <<
               "\\nrefs:" << resource.mRefCount
               <<"\""
               ", style=filled, fillcolor="
               << (resource.mRefCount ? "skyblue" : "skyblue4") << "]\n";
    }

    out << "\n";
    for (auto const& resource : registry) {
        if (removeCulled && !resource.mRefCount) continue;
        auto const& writers = resource.mWriters;
        size_t version = 0;
        for (auto const& writer : writers) {
            if (removeCulled && !writer->mRefCount) continue;
            out << "\"" << writer->getName() << "\" -> { ";
            out << "\"" << resource.getName() << "\\n(version: " << version++ << ")\"";
            out << "} [color=red2]\n";
        }
    }

    out << "\n";
    for (auto const& id : resourcesIds) {
        auto const& resource = registry[id.index];
        if (removeCulled && !resource.mRefCount) continue;
        out << "\"" << resource.getName() << "\\n(version: " << id.version << ")\"";
        out << " -> {";
        // who reads us...
        for (auto const& pass : mFrameGraphPasses) {
            if (removeCulled && !pass->mRefCount) continue;
            for (auto const& pass_id : pass->mReads) {
                if (pass_id.index == id.index && pass_id.version == id.version ) {
                    out << "\"" << pass->getName() << "\"";
                }
            }
        }
        out << "} [color=lightgreen]\n";
    }
    out << "}" << utils::io::endl;
}

} // namespace filament
