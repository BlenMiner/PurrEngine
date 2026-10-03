#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static void frame(void)
{
    tide_draw_reset(&draw);
    tide_frame(&world, NULL, 1.0f, &local, &draw, &gui);
}

static const tide_draw_command *command(const uint32_t i)
{
    return &draw.commands[i];
}

static const tide_draw_mesh_data *mesh_of(const uint32_t i)
{
    return &draw.meshes[command(i)->mesh3.mesh - 1u];
}

TIDE_TEST(meshes_3d_draw_through_their_camera)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    tide_local_init(&local);
    frame();
    TIDE_REQUIRE(draw.count == 8 && draw.mesh_count == 2 && draw.texture_count == 1);

    // The perspective camera, and a mesh of lists made along the way
    TIDE_CHECK(command(0)->kind == TIDE_DRAW_CLEAR);
    TIDE_CHECK(command(1)->kind == TIDE_DRAW_CAMERA_3D && command(1)->camera.fit == 1);
    TIDE_CHECK(command(2)->kind == TIDE_DRAW_MESH_3D && command(2)->mesh3.count == 1 && command(2)->mesh3.texture == 0);
    TIDE_CHECK(draw.matrices[command(2)->mesh3.first].c3.z == 10.0f);
    TIDE_CHECK(mesh_of(2)->vertex_count == 3 && mesh_of(2)->vertices[1].position.x == 1.0f);
    TIDE_CHECK(mesh_of(2)->vertices[0].color.r == 1.0f && mesh_of(2)->vertices[0].color.a == 1.0f); // White, left out

    // Each tree, one command, with the grid as the texture
    const tide_draw_command *trees = command(3);
    TIDE_CHECK(trees->kind == TIDE_DRAW_MESH_3D && trees->mesh3.count == 3 && trees->mesh3.texture == 1);
    TIDE_CHECK(trees->mesh3.filter == TIDE_FILTER_POINT);
    const tide_float4x4 *tall = NULL;
    for (uint32_t i = 0; i < 3; i++) {
        if (draw.matrices[trees->mesh3.first + i].c1.y == 2.0f) tall = &draw.matrices[trees->mesh3.first + i];
    }
    TIDE_CHECK(tall && tall->c3.x == -1.0f && tall->c3.z == 5.0f);
    TIDE_CHECK(mesh_of(3)->vertex_count == 4 && mesh_of(3)->index_count == 6);
    TIDE_CHECK(mesh_of(3)->vertices[1].uv.x == 1.0f && mesh_of(3)->vertices[1].color.g == 1.0f);
    TIDE_CHECK(draw.textures[0].pixels[0] == 255 && draw.textures[0].pixels[1] == 0);

    // Any projection: the same mesh, sampled another way
    TIDE_CHECK(command(4)->kind == TIDE_DRAW_CAMERA_3D && command(4)->camera.fit == 0);
    TIDE_CHECK(command(5)->kind == TIDE_DRAW_MESH_3D && command(5)->mesh3.mesh == trees->mesh3.mesh);
    TIDE_CHECK(command(5)->mesh3.filter == TIDE_FILTER_BILINEAR);
    // From 10 above, looking down: the origin is in the middle of the screen
    const tide_float4 origin = tide_mul_f4x4_f4(draw.matrices[command(4)->camera.matrix], tide_f4(0, 0, 0, 1));
    TIDE_CHECK(origin.x * origin.x < 1e-8f && origin.y * origin.y < 1e-8f && origin.w > 9.9f && origin.w < 10.1f);
    TIDE_CHECK(command(6)->kind == TIDE_DRAW_CAMERA && command(7)->kind == TIDE_DRAW_RECT);
}

TIDE_TEST(meshes_3d_kept_until_they_change)
{
    // A frame where nothing changed copies nothing
    const uint32_t kept = command(3)->mesh3.mesh;
    draw.meshes[kept - 1u].vertices[0].position.y = 77.0f;
    frame();
    TIDE_REQUIRE(draw.count == 8 && command(3)->mesh3.mesh == kept);
    TIDE_CHECK(mesh_of(3)->vertices[0].position.y == 77.0f && draw.mesh_count == 2);

    // The view added a corner
    frame();
    TIDE_REQUIRE(draw.count == 8);
    TIDE_CHECK(command(3)->mesh3.mesh != kept && mesh_of(3)->vertex_count == 5);
    TIDE_CHECK(mesh_of(3)->vertices[0].position.y == 0.0f && mesh_of(5)->vertex_count == 5);
}
