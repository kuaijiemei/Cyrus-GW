from fastapi import FastAPI

from app.api import router as api_router
from app.schemas import HealthResponse

app = FastAPI(title="Cyrus-GW Agent", version="0.1.0")
app.include_router(api_router)


@app.get("/health", response_model=HealthResponse)
async def health() -> HealthResponse:
    return HealthResponse(status="ok", service="agent")
