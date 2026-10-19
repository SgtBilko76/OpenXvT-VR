/* Fullscreen triangle for analytic hyperspace backgrounds. */

struct VertexOutput {
	float4 position : SV_Position;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
	/* (-1,-1), (-1,3), (3,-1) computed instead of a `static const` table:
	 * Adreno (Quest) drivers read SPIR-V Private initialisers as zero. */
	float2 position = float2(vertex_id == 2u ? 3.0f : -1.0f, vertex_id == 1u ? 3.0f : -1.0f);

	VertexOutput output;
	output.position = float4(position, 0.0f, 1.0f);
	return output;
}
