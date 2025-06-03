from fastapi import FastAPI, APIRouter, HTTPException, Depends
from dotenv import load_dotenv
from starlette.middleware.cors import CORSMiddleware
from motor.motor_asyncio import AsyncIOMotorClient
import os
import logging
from pathlib import Path
from pydantic import BaseModel, Field
from typing import List, Optional, Dict, Any
import uuid
from datetime import datetime
import hashlib
import json

ROOT_DIR = Path(__file__).parent
load_dotenv(ROOT_DIR / '.env')

# MongoDB connection
mongo_url = os.environ['MONGO_URL']
client = AsyncIOMotorClient(mongo_url)
db = client[os.environ['DB_NAME']]

# Create the main app without a prefix
app = FastAPI()

# Create a router with the /api prefix
api_router = APIRouter(prefix="/api")

# Models
class User(BaseModel):
    id: str = Field(default_factory=lambda: str(uuid.uuid4()))
    username: str
    password_hash: str
    created_at: datetime = Field(default_factory=datetime.utcnow)
    desktop_settings: Dict[str, Any] = Field(default_factory=dict)

class UserCreate(BaseModel):
    username: str
    password: str

class UserLogin(BaseModel):
    username: str
    password: str

class FileItem(BaseModel):
    id: str = Field(default_factory=lambda: str(uuid.uuid4()))
    user_id: str
    name: str
    type: str  # 'file' or 'folder'
    content: Optional[str] = None  # file content (base64 for binary files)
    parent_id: Optional[str] = None  # folder parent
    path: str  # full path
    size: int = 0
    created_at: datetime = Field(default_factory=datetime.utcnow)
    modified_at: datetime = Field(default_factory=datetime.utcnow)
    is_deleted: bool = False  # for recycle bin
    file_extension: Optional[str] = None

class FileCreate(BaseModel):
    name: str
    type: str
    content: Optional[str] = None
    parent_id: Optional[str] = None
    path: str

class FileUpdate(BaseModel):
    name: Optional[str] = None
    content: Optional[str] = None

class DesktopSettings(BaseModel):
    wallpaper: Optional[str] = None
    window_positions: Dict[str, Any] = Field(default_factory=dict)
    taskbar_settings: Dict[str, Any] = Field(default_factory=dict)
    theme: Optional[str] = None

# Utility functions
def hash_password(password: str) -> str:
    return hashlib.sha256(password.encode()).hexdigest()

def verify_password(password: str, hashed: str) -> bool:
    return hash_password(password) == hashed

def get_file_extension(filename: str) -> str:
    """Get file extension from filename"""
    if '.' in filename:
        return filename.split('.')[-1].lower()
    return ''

# Authentication routes
@api_router.post("/register")
async def register_user(user_data: UserCreate):
    # Check if user exists
    existing_user = await db.users.find_one({"username": user_data.username})
    if existing_user:
        raise HTTPException(status_code=400, detail="Username already exists")
    
    # Create user
    user = User(
        username=user_data.username,
        password_hash=hash_password(user_data.password),
        desktop_settings={
            "wallpaper": "classic_clouds",
            "window_positions": {},
            "taskbar_settings": {"position": "bottom"},
            "theme": "classic"
        }
    )
    
    await db.users.insert_one(user.dict())
    
    # Create default folders for user
    folders = [
        {"name": "Documents", "path": "/Documents"},
        {"name": "Desktop", "path": "/Desktop"},
        {"name": "Downloads", "path": "/Downloads"},
        {"name": "Programs", "path": "/Programs"},
        {"name": "Recycle Bin", "path": "/RecycleBin"}
    ]
    
    for folder in folders:
        file_item = FileItem(
            user_id=user.id,
            name=folder["name"],
            type="folder",
            path=folder["path"]
        )
        await db.files.insert_one(file_item.dict())
    
    return {"message": "User created successfully", "user_id": user.id}

@api_router.post("/login")
async def login_user(login_data: UserLogin):
    user = await db.users.find_one({"username": login_data.username})
    if not user or not verify_password(login_data.password, user["password_hash"]):
        raise HTTPException(status_code=401, detail="Invalid credentials")
    
    return {
        "message": "Login successful", 
        "user_id": user["id"],
        "username": user["username"],
        "desktop_settings": user.get("desktop_settings", {})
    }

@api_router.get("/user/{user_id}")
async def get_user(user_id: str):
    user = await db.users.find_one({"id": user_id})
    if not user:
        raise HTTPException(status_code=404, detail="User not found")
    
    return {
        "id": user["id"],
        "username": user["username"],
        "desktop_settings": user.get("desktop_settings", {})
    }

@api_router.put("/user/{user_id}/desktop-settings")
async def update_desktop_settings(user_id: str, settings: DesktopSettings):
    result = await db.users.update_one(
        {"id": user_id},
        {"$set": {"desktop_settings": settings.dict()}}
    )
    if result.matched_count == 0:
        raise HTTPException(status_code=404, detail="User not found")
    
    return {"message": "Desktop settings updated"}

# File system routes
@api_router.get("/files/{user_id}")
async def get_files(user_id: str, parent_id: Optional[str] = None, path: Optional[str] = None, include_deleted: bool = False):
    query = {"user_id": user_id}
    if not include_deleted:
        query["is_deleted"] = {"$ne": True}
    
    # Handle parent_id parameter
    if parent_id and parent_id != "null":
        query["parent_id"] = parent_id
    elif path:
        query["path"] = {"$regex": f"^{path}"}
    else:
        # For root directory or when parent_id is None/"null"
        query["parent_id"] = None
    
    files = await db.files.find(query).to_list(1000)
    return [FileItem(**file) for file in files]

@api_router.get("/files/{user_id}/recycle-bin")
async def get_deleted_files(user_id: str):
    """Get files in recycle bin"""
    files = await db.files.find({"user_id": user_id, "is_deleted": True}).to_list(1000)
    return [FileItem(**file) for file in files]

@api_router.post("/files/{user_id}")
async def create_file(user_id: str, file_data: FileCreate):
    # Check if file already exists in the same path
    existing = await db.files.find_one({
        "user_id": user_id,
        "name": file_data.name,
        "parent_id": file_data.parent_id,
        "is_deleted": {"$ne": True}
    })
    if existing:
        raise HTTPException(status_code=400, detail="File already exists")
    
    file_extension = get_file_extension(file_data.name)
    
    file_item = FileItem(
        user_id=user_id,
        name=file_data.name,
        type=file_data.type,
        content=file_data.content or "",
        parent_id=file_data.parent_id,
        path=file_data.path,
        size=len(file_data.content or ""),
        file_extension=file_extension
    )
    
    await db.files.insert_one(file_item.dict())
    return file_item

@api_router.get("/files/{user_id}/{file_id}")
async def get_file(user_id: str, file_id: str):
    file_item = await db.files.find_one({"id": file_id, "user_id": user_id})
    if not file_item:
        raise HTTPException(status_code=404, detail="File not found")
    
    return FileItem(**file_item)

@api_router.put("/files/{user_id}/{file_id}")
async def update_file(user_id: str, file_id: str, file_update: FileUpdate):
    update_data = {"modified_at": datetime.utcnow()}
    if file_update.name:
        update_data["name"] = file_update.name
        update_data["file_extension"] = get_file_extension(file_update.name)
    if file_update.content is not None:
        update_data["content"] = file_update.content
        update_data["size"] = len(file_update.content)
    
    result = await db.files.update_one(
        {"id": file_id, "user_id": user_id},
        {"$set": update_data}
    )
    
    if result.matched_count == 0:
        raise HTTPException(status_code=404, detail="File not found")
    
    return {"message": "File updated successfully"}

@api_router.delete("/files/{user_id}/{file_id}")
async def delete_file(user_id: str, file_id: str, permanent: bool = False):
    if permanent:
        # Permanent delete
        result = await db.files.delete_one({"id": file_id, "user_id": user_id})
        if result.deleted_count == 0:
            raise HTTPException(status_code=404, detail="File not found")
        return {"message": "File permanently deleted"}
    else:
        # Move to recycle bin
        result = await db.files.update_one(
            {"id": file_id, "user_id": user_id},
            {"$set": {"is_deleted": True, "modified_at": datetime.utcnow()}}
        )
        if result.matched_count == 0:
            raise HTTPException(status_code=404, detail="File not found")
        return {"message": "File moved to recycle bin"}

@api_router.post("/files/{user_id}/{file_id}/restore")
async def restore_file(user_id: str, file_id: str):
    """Restore file from recycle bin"""
    result = await db.files.update_one(
        {"id": file_id, "user_id": user_id, "is_deleted": True},
        {"$set": {"is_deleted": False, "modified_at": datetime.utcnow()}}
    )
    if result.matched_count == 0:
        raise HTTPException(status_code=404, detail="File not found in recycle bin")
    
    return {"message": "File restored successfully"}

@api_router.post("/files/{user_id}/empty-recycle-bin")
async def empty_recycle_bin(user_id: str):
    """Permanently delete all files in recycle bin"""
    result = await db.files.delete_many({"user_id": user_id, "is_deleted": True})
    return {"message": f"Permanently deleted {result.deleted_count} files"}

# System routes
@api_router.get("/")
async def root():
    return {"message": "RetroOS Backend API"}

@api_router.get("/system/info")
async def system_info():
    return {
        "os_name": "RetroOS",
        "version": "1.0",
        "build": "95/98 Classic Enhanced",
        "uptime": "Running since startup",
        "features": [
            "Window Management",
            "File System", 
            "User Accounts",
            "Paint Program",
            "Calculator",
            "Text Editor",
            "Web Browser",
            "Games Launcher",
            "Recycle Bin",
            "Control Panel"
        ]
    }

@api_router.get("/wallpapers")
async def get_available_wallpapers():
    """Get list of available wallpapers"""
    wallpapers = [
        {"id": "classic_clouds", "name": "Classic Clouds", "description": "Windows 95 style clouds"},
        {"id": "teal_solid", "name": "Teal", "description": "Solid teal background"},
        {"id": "green_circuit", "name": "Green Circuit", "description": "Circuit board pattern"},
        {"id": "blue_space", "name": "Blue Space", "description": "Deep space background"},
        {"id": "pattern_maze", "name": "Maze Pattern", "description": "Classic maze pattern"},
        {"id": "gradient_sunset", "name": "Sunset Gradient", "description": "Orange sunset gradient"}
    ]
    return wallpapers

# Include the router in the main app
app.include_router(api_router)

app.add_middleware(
    CORSMiddleware,
    allow_credentials=True,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

@app.on_event("shutdown")
async def shutdown_db_client():
    client.close()
